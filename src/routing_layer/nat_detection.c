/* Владелец измеряет рабочий UDP endpoint по разным IP. Наблюдения возвращаются
 * через router, независимо от прямого UDP-ответа. Входящие пробы не получают PONG;
 * незнакомость фактического source IP проверяет только получатель. */
#include "nat_detection.h"
#include "chat/chat_event.h"

#include <stdio.h>
#include <string.h>
#include "utun_instance.h"
#include "config_parser.h"
#include "etcp_api.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "packet_transport.h"
#include "stcp_link.h"
#include "route_ping.h"
#include "topo_node.h"
#include "topo_group.h"
#include "etcp_router.h"
#include "route_crypto.h"
#include "stun_nat.h"
#include "sock_match.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../lib/db_sql.h"

#define NAT_WIRE_VERSION 5
#define NAT_MAX_BINDINGS 128
#define NAT_MAX_PROBES 64
#define NAT_MAX_CONTACTS 256
#define NAT_TICK_TB 1000
#define NAT_ROUND_TB 300000
#define NAT_REFRESH_TB 18000000
#define NAT_RETRY_TB 300000
#define NAT_FAILURE_RECHECK_TB 3000000
#define NAT_LINK_BOOTSTRAP_TB 20000
#define NAT_PROBE_COUNT 3
#define NAT_QUERY_TAG_SIZE (ROUTE_OBSERVE_TAG_SIZE + 9)

enum nat_command { NAT_RUN = 1, NAT_RESULT, NAT_RECHECK, NAT_OBSERVATION };
enum nat_phase { NAT_IDLE, NAT_OBSERVE_B, NAT_PREPARE_C, NAT_PRE, NAT_OBSERVE_C,
                 NAT_POST, NAT_REPEAT_B, NAT_REPEAT_C, NAT_PRE_WAIT, NAT_POST_WAIT };
enum nat_step { NAT_STEP_PREPARE = 1, NAT_STEP_PRE, NAT_STEP_POST, NAT_STEP_HELPER_OBSERVE = 128 };
enum nat_endpoint_kind { NAT_ENDPOINT_PUBLIC, NAT_ENDPOINT_ANY, NAT_ENDPOINT_DIRECT };

/* Только байты: формат независим от alignment/endian платформы. */
struct nat_wire {
    uint8_t cmd, command, version, owner_socket, helper_socket, step, reason, sent, received, errors;
    uint8_t owner[32], observer[32], helper[32]; /* Ed25519 участников пробы. */
    uint8_t generation[8], round[8], token[8], timeout_ms[2];
    uint8_t target[ROUTE_ADDR_WIRE_SIZE], primary[ROUTE_ADDR_WIRE_SIZE];
    uint8_t observer_address[ROUTE_ADDR_WIRE_SIZE];
};

struct nat_pair {
    uint64_t group;
    uint8_t observer[32], helper[32];
    uint8_t helper_priority; /* Публичные IP: свежий/знакомый; затем приватные: свежий/знакомый; затем один B. */
    uint8_t observer_endpoint[ROUTE_ADDR_WIRE_SIZE], helper_endpoint[ROUTE_ADDR_WIRE_SIZE];
};
struct nat_recheck_wire { uint8_t cmd, command, version, socket_id, address[ROUTE_ADDR_WIRE_SIZE]; };

struct nat_socket {
    struct nat_socket *next;
    struct NAT_DETECTION *detector;
    struct ETCP_SOCKET *socket, *helper_primary;
    struct sockaddr_storage local, observer_address, helper_address;
    struct sockaddr_storage baseline, helper_seen;
    struct sockaddr_storage measured_address, observed_address;
    struct address_profile measured_profile;
    uint64_t select_due, available_fingerprint, reservation_fingerprint, cold_until, failure_recheck_tb, history_retry_tb;
    uint8_t observer_key[32], helper_key[32];
    uint8_t observer_ed[32], helper_ed[32]; /* Участники последней начатой попытки, включая её итог. */
    uint64_t group_id, round, token, observation_token, deadline, due, completed_tb;
    uint8_t preferred_observer[32];
    uint64_t cycle_deadline, heartbeat_due, heartbeat_received, pre_started;
    struct nat_pair cursor, current_pair; /* Курсор продвигается только после завершённой попытки. */
    uint8_t contacts[NAT_MAX_CONTACTS][17];
    unsigned contact_count, contact_cutoff, cycle_contact_cutoff, phase, reason, retries;
    uint8_t history_loaded, history_complete, history_incomplete, filter_clean, pre_received, post_received, pre_healthy;
    uint8_t history_pending, history_marker_pending, history_marker_saved;
    uint8_t cycle_history_complete, cycle_mapping_seen, pair_full_budget;
    uint8_t helper_socket, active_ping, heartbeat_active, forced, cycling;
    uint32_t rounds, helper_reselections;
};

struct nat_binding {
    struct nat_binding *next;
    uint64_t group_id, generation, round, expires, token, observer_generation;
    uint8_t owner_socket, observer_socket, owner[32], helper[32];
    struct sockaddr_storage target;
};

struct nat_probe {
    struct nat_probe *next;
    struct NAT_DETECTION *detector;
    struct nat_wire message;
    uint64_t group_id, source_generation, io_generation, observer_generation, due, expires;
    uint8_t reply[32], owner_key[32], observer_key[32];
    uint8_t source_socket, io_socket, observer_socket, active, cancelled, finding_primary;
};

struct NAT_DETECTION {
    struct UTUN_INSTANCE *instance;
    struct nat_socket *sockets;
    struct nat_binding *bindings;
    struct nat_probe *probes;
    void *timer;
    void *topology_wake;
    uint64_t publish_after;
    unsigned history_refs; /* Владелец и принятые/отказываемые запросы БД; изменяется только в uasync. */
    uint8_t allow_local, stopping, automatic, publish_pending, reservation_dirty;
};

static void owner_ping_cb(int, uint16_t, void *, uint64_t, const uint8_t *, size_t);
static void owner_heartbeat_cb(int, uint16_t, void *, uint64_t, const uint8_t *, size_t);
static void helper_ping_cb(int, uint16_t, void *, uint64_t, const uint8_t *, size_t);
static void detector_tick(void *);
static int owner_observe(struct nat_socket *);
static int owner_run(struct nat_socket *, uint8_t);
static void owner_finish(struct nat_socket *, uint8_t);
static void helper_remove(struct nat_probe *);
static int helper_send(struct nat_probe *);
static void owner_start(struct nat_socket *);
static int contacted(struct nat_socket *, const struct sockaddr_storage *);

/* Не сбрасывает последний успех; повторные ошибки одного endpoint объединяются и ограничиваются по частоте. */
static int schedule_failure_recheck(struct nat_socket *state, const uint8_t peer[32], int remote) {
    uint64_t now = get_time_tb();
    if (!state || !state->detector->automatic || state->phase || state->cycling || state->forced ||
        (state->failure_recheck_tb && now - state->failure_recheck_tb < NAT_FAILURE_RECHECK_TB)) return 0;
    state->failure_recheck_tb = now; state->due = now; state->forced = 1; memset(state->preferred_observer, 0, 32);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT failure recheck scheduled socket=%s generation=%016llx peer=%s remote_request=%d",
               state->socket->name, (unsigned long long)state->socket->generation, sc_public_key_hex(peer, (char[65]){0}), remote);
    return 1;
}

static uint64_t read64(const uint8_t *data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value = (value << 8) | data[i];
    return value;
}
static void write64(uint8_t *data, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) data[i] = (uint8_t)(value >> (56 - i * 8));
}
static int random_id(uint64_t *value) {
    do {
        if (random_bytes((uint8_t *)value, sizeof(*value))) {
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT round/token RNG failed"); return -1;
        }
    } while (!*value);
    return 0;
}

const char *nat_check_reason_name(uint8_t reason) {
    static const char *const names[] = {"none", "no_observer", "no_helper", "control_lost", "send_error", "timeout",
        "address_changed", "contact_history", "proxy", "no_listener", "cancelled", "invalid_observation", "dependent_mapping",
        "cycle_budget"};
    return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "invalid_reason";
}

static struct ETCP_SOCKET *find_socket(struct NAT_DETECTION *detector, uint8_t id, uint64_t generation) {
    for (struct ETCP_SOCKET *socket = detector->instance->etcp_sockets; socket; socket = socket->next)
        if (socket->sock_id == id && (!generation || socket->generation == generation)) return socket;
    return NULL;
}

static int public_address(struct NAT_DETECTION *detector, const struct sockaddr_storage *address) {
    if (address->ss_family == AF_INET) {
        uint32_t ip = ((const struct sockaddr_in *)address)->sin_addr.s_addr;
        return ip && (ip_is_public(ip) || detector->allow_local);
    }
    if (address->ss_family == AF_INET6) {
        const uint8_t *ip = ((const struct sockaddr_in6 *)address)->sin6_addr.s6_addr;
        return (ip[0] & 0xe0) == 0x20 || (detector->allow_local && !memcmp(ip, &in6addr_loopback, 16));
    }
    return 0;
}

static int direct_udp_socket(struct NAT_DETECTION *detector, const struct ETCP_SOCKET *socket) {
    return socket && !socket->is_tcp && !socket->probe_only && !socket->socks_udp && !socket->only_local &&
        socket->type != CFG_SERVER_TYPE_LOCAL && socket->type != CFG_SERVER_TYPE_PRIVATE &&
        public_address(detector, &socket->local_addr);
}

static int key_empty(const uint8_t ed[32]) {
    static const uint8_t zero[32];
    return !memcmp(ed, zero, 32);
}

static int single_peer_pair(const struct nat_pair *pair) {
    return !memcmp(pair->observer, pair->helper, 32);
}

/* Подписанная идентичность участника выбранной группы. Bootstrap не разрешает управление. */
static const struct TOPO_NODE *group_node(struct NAT_DETECTION *detector, uint64_t group_id, const uint8_t ed[32]) {
    struct UTUN_INSTANCE *inst = detector->instance;
    struct TOPO_GROUP *group = topo_groups_find(inst->topo_groups, group_id);
    if (!group || group->stopping) return NULL;
    const struct TOPO_GROUP_NODE *member =
        !memcmp(ed, inst->my_ed25519_pubkey, 32) ? group->local_node : topo_node_find_by_ed(group, ed);
    if (!member || !member->identity || !member->identity->timestamp) return NULL;
    if (group->group_type != TOPO_GROUP_TYPE_UTUN && !topo_group_chat_key_allowed(inst, group_id, ed)) return NULL;
    return member->identity;
}

static const struct TOPO_NODE *group_node_x(struct NAT_DETECTION *detector, uint64_t group, const uint8_t x[32]) {
    const struct TOPO_NODE *node = topo_node_registry_lookup(detector->instance->topo_groups, sc_derive_node_id_from_pubkey(x), x);
    return node && node == group_node(detector, group, node->ed25519_public_key) ? node : NULL;
}

/* Повторно проверяем обе роли после смены состава. Нулевой ключ помощника означает измерение с одним наблюдателем. */
static int participants_allowed(struct NAT_DETECTION *detector, uint64_t group_id, const uint8_t first[32], const uint8_t second[32]) {
    struct TOPO_GROUP *group = topo_groups_find(detector->instance->topo_groups, group_id);
    if (group && group->group_type == TOPO_GROUP_TYPE_UTUN) return 1;
    if (topo_group_chat_key_allowed(detector->instance, group_id, first) &&
        (key_empty(second) || topo_group_chat_key_allowed(detector->instance, group_id, second))) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT CHAT participants denied group=%016llx first=%s second=%s",
               (unsigned long long)group_id, sc_public_key_hex(first, (char[65]){0}), sc_public_key_hex(second, (char[65]){0}));
    return 0;
}

/* Управление допускает транзит и TCP по готовой сессии выбранной группы. */
static int control_route(struct NAT_DETECTION *detector, uint64_t group_id, const uint8_t ed[32]) {
    return group_node(detector, group_id, ed) &&
        etcp_router_group_peer_ready(detector->instance, group_id, ed, ETCP_RT_ID_NAT_DETECTION);
}

/* Для пары одного пира подходят только публичные интерфейсные адреса из NODEINFO. */
static int node_udp_endpoint(struct NAT_DETECTION *detector, const struct TOPO_NODE *node, int family, enum nat_endpoint_kind kind,
                             const uint8_t after[ROUTE_ADDR_WIRE_SIZE], struct sockaddr_storage *address, uint8_t *socket_id,
                             int *is_direct) {
    uint8_t best[ROUTE_ADDR_WIRE_SIZE] = {0};
    if (!node) return 0;
    for (const struct TOPO_ADDR4 *a = family == AF_INET ? node->v4_addrs : NULL; a; a = a->next) {
        if (!(a->protocol & TOPO_PROTO_UDP) || !a->port) continue;
        const struct TOPO_SOCKMETA4 *meta = node->v4_sock_meta;
        while (meta && meta->id != a->socket_id) meta = meta->next;
        if (meta && (meta->config_type == CFG_SERVER_TYPE_PRIVATE || meta->config_type == CFG_SERVER_TYPE_LOCAL)) continue;
        struct sockaddr_storage candidate = {0}; struct sockaddr_in *v4 = (struct sockaddr_in *)&candidate;
        v4->sin_family = AF_INET; memcpy(&v4->sin_addr, a->addr, 4); v4->sin_port = htons(a->port);
        uint8_t wire[ROUTE_ADDR_WIRE_SIZE];
        int global = public_address(detector, &candidate);
        int direct = global && meta && a->type == TOPO_ADDR_INTERFACE;
        if ((kind == NAT_ENDPOINT_DIRECT && !direct) || (kind == NAT_ENDPOINT_PUBLIC && !global) ||
            route_ping_addr_encode(wire, &candidate)) continue;
        if (after && memcmp(wire, after, sizeof(wire)) <= 0) continue;
        int order = memcmp(wire, best, sizeof(wire));
        if (best[0] && (order > 0 || (!order && !(is_direct && direct && !*is_direct)))) continue;
        memcpy(best, wire, sizeof(best)); *address = candidate; *socket_id = a->socket_id;
        if (is_direct) *is_direct = direct;
    }
    for (const struct TOPO_ADDR6 *a = family == AF_INET6 ? node->v6_addrs : NULL; a; a = a->next) {
        if (!(a->protocol & TOPO_PROTO_UDP) || !a->port) continue;
        const struct TOPO_SOCKMETA6 *meta = node->v6_sock_meta;
        while (meta && meta->id != a->socket_id) meta = meta->next;
        if (meta && (meta->config_type == CFG_SERVER_TYPE_PRIVATE || meta->config_type == CFG_SERVER_TYPE_LOCAL)) continue;
        struct sockaddr_storage candidate = {0}; struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&candidate;
        v6->sin6_family = AF_INET6; memcpy(&v6->sin6_addr, a->addr, 16); v6->sin6_port = htons(a->port);
        uint8_t wire[ROUTE_ADDR_WIRE_SIZE];
        int global = public_address(detector, &candidate);
        int direct = global && meta && a->type == TOPO_ADDR_INTERFACE;
        if ((kind == NAT_ENDPOINT_DIRECT && !direct) || (kind == NAT_ENDPOINT_PUBLIC && !global) ||
            route_ping_addr_encode(wire, &candidate)) continue;
        if (after && memcmp(wire, after, sizeof(wire)) <= 0) continue;
        int order = memcmp(wire, best, sizeof(wire));
        if (best[0] && (order > 0 || (!order && !(is_direct && direct && !*is_direct)))) continue;
        memcpy(best, wire, sizeof(best)); *address = candidate; *socket_id = a->socket_id;
        if (is_direct) *is_direct = direct;
    }
    if (kind == NAT_ENDPOINT_DIRECT) return best[0] != 0;
    /* Наблюдаемый адрес уже работающего UDP помогает bootstrap за NAT; прямой линк не является условием отбора. */
    struct ETCP_CONN *connection = instance_find_conn_by_key(detector->instance, node->public_key);
    for (struct ETCP_LINK *link = connection ? connection->links : NULL; link; link = link->next) {
        if (link->is_tcp || !link->initialized || !link->link_status || link->remote_only_local ||
            link->remote_addr.ss_family != family || !public_address(detector, &link->remote_addr)) continue;
        uint8_t wire[ROUTE_ADDR_WIRE_SIZE];
        if (route_ping_addr_encode(wire, &link->remote_addr)) continue;
        if ((after && memcmp(wire, after, sizeof(wire)) <= 0) || (best[0] && memcmp(wire, best, sizeof(wire)) >= 0)) continue;
        memcpy(best, wire, sizeof(best)); *address = link->remote_addr; *socket_id = link->remote_socket_id;
        if (is_direct) *is_direct = 0;
    }
    return best[0] != 0;
}

static int pair_after(struct nat_pair a, struct nat_pair b) {
    if (!a.group || !b.group) return a.group != 0;
    if (a.helper_priority != b.helper_priority) return a.helper_priority > b.helper_priority;
    int a_shared = single_peer_pair(&a), b_shared = single_peer_pair(&b);
    if (a_shared != b_shared) return a_shared > b_shared;
    if (a.group != b.group) return a.group > b.group;
    int order = memcmp(a.observer, b.observer, 32);
    if (order) return order > 0;
    order = memcmp(a.helper, b.helper, 32);
    if (order) return order > 0;
    order = memcmp(a.observer_endpoint, b.observer_endpoint, sizeof(a.observer_endpoint));
    return order ? order > 0 : memcmp(a.helper_endpoint, b.helper_endpoint, sizeof(a.helper_endpoint)) > 0;
}

/* Обходит пары всех групп без ограничения числа узлов и без хранения указателей на топологию. */
static int select_pair(struct nat_socket *state) {
    struct NAT_DETECTION *detector = state->detector;
    struct nat_pair best = {0};
    struct nat_pair single = {0};
    struct sockaddr_storage single_address = {0}; uint8_t single_key[32] = {0};
    struct sockaddr_storage observer_address = {0}, helper_address = {0};
    uint8_t observer_key[32] = {0}, helper_key[32] = {0}, helper_socket = 0;
    unsigned observers = 0, groups = 0;
    for (struct ll_entry *e = detector->instance->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP *group = (struct TOPO_GROUP *)e;
        if (group->stopping || !group->local_node) continue;
        groups++;
        for (struct ll_entry *b = group->nodes->head; b; b = b->next) {
            const uint8_t *observer = ((struct TOPO_GROUP_NODE *)b)->ed25519;
            if (!memcmp(observer, detector->instance->my_ed25519_pubkey, 32) ||
                (!key_empty(state->preferred_observer) && memcmp(observer, state->preferred_observer, 32)) ||
                !control_route(detector, group->group_id, observer)) continue;
            const struct TOPO_NODE *bn = group_node(detector, group->group_id, observer);
            uint8_t previous_b[ROUTE_ADDR_WIRE_SIZE] = {0};
            struct sockaddr_storage ba = {0}; uint8_t bs = 0; int direct_b;
            int more_b = node_udp_endpoint(detector, bn, state->local.ss_family, NAT_ENDPOINT_PUBLIC, previous_b, &ba, &bs, &direct_b);
            for (; more_b; more_b =
                 node_udp_endpoint(detector, bn, state->local.ss_family, NAT_ENDPOINT_PUBLIC, previous_b, &ba, &bs, &direct_b)) {
                if (route_ping_addr_encode(previous_b, &ba)) break;
                observers++;
                struct nat_pair observer_only = {.group = group->group_id, .helper_priority = 4};
                memcpy(observer_only.observer, observer, 32);
                memcpy(observer_only.observer_endpoint, previous_b, sizeof(previous_b));
                if (pair_after(observer_only, state->cursor) &&
                    (!single.group || pair_after(single, observer_only))) {
                    single = observer_only; single_address = ba; memcpy(single_key, bn->public_key, 32);
                }
                for (struct ll_entry *c = group->nodes->head; c; c = c->next) {
                    const uint8_t *helper = ((struct TOPO_GROUP_NODE *)c)->ed25519;
                    int shared = !memcmp(helper, observer, 32);
                    if (!memcmp(helper, detector->instance->my_ed25519_pubkey, 32) ||
                        (shared ? !direct_b : !control_route(detector, group->group_id, helper))) continue;
                    const struct TOPO_NODE *cn = shared ? bn : group_node(detector, group->group_id, helper);
                    enum nat_endpoint_kind kind = shared ? NAT_ENDPOINT_DIRECT : NAT_ENDPOINT_ANY;
                    struct sockaddr_storage ca = {0}; uint8_t cs = 0, previous_c[ROUTE_ADDR_WIRE_SIZE] = {0};
                    int more_c = node_udp_endpoint(detector, cn, state->local.ss_family, kind, previous_c, &ca, &cs, NULL);
                    for (; more_c; more_c =
                         node_udp_endpoint(detector, cn, state->local.ss_family, kind, previous_c, &ca, &cs, NULL)) {
                        if (route_ping_addr_encode(previous_c, &ca)) break;
                        if (shared && (bs == cs || route_ping_addr_equal(&ba, &ca, 0))) continue;
                        int old_contact = !state->cycle_history_complete;
                        for (unsigned i = 0; i < state->cycle_contact_cutoff; i++)
                            if (!memcmp(state->contacts[i], previous_c, 17)) { old_contact = 1; break; }
                        struct nat_pair candidate = {.group = group->group_id,
                            .helper_priority = 2 * !public_address(detector, &ca) + old_contact};
                        memcpy(candidate.observer, observer, 32); memcpy(candidate.helper, helper, 32);
                        memcpy(candidate.observer_endpoint, previous_b, sizeof(previous_b));
                        memcpy(candidate.helper_endpoint, previous_c, sizeof(previous_c));
                        if (!pair_after(candidate, state->cursor)) continue;
                        if (best.group && !pair_after(best, candidate)) continue;
                        best = candidate; observer_address = ba; helper_address = ca; helper_socket = cs;
                        memcpy(observer_key, bn->public_key, 32); memcpy(helper_key, cn->public_key, 32);
                    }
                }
            }
        }
    }
    if (!best.group && single.group && !state->cycle_mapping_seen) {
        best = single; observer_address = single_address; memcpy(observer_key, single_key, 32);
    }
    if (!best.group) {
        if (!observers || !state->cursor.group || !state->reason)
            state->reason = observers ? NAT_REASON_NO_HELPER : NAT_REASON_NO_OBSERVER;
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT candidate cycle exhausted socket=%s groups=%u observers=%u reason=%s",
                    state->socket->name, groups, observers, nat_check_reason_name(state->reason));
        return -1;
    }
    state->current_pair = best;
    memcpy(state->observer_ed, best.observer, 32); memcpy(state->helper_ed, best.helper, 32);
    state->group_id = best.group;
    state->observer_address = observer_address; state->helper_address = helper_address; state->helper_socket = helper_socket;
    memcpy(state->observer_key, observer_key, 32); memcpy(state->helper_key, helper_key, 32);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT candidates socket=%s group=%016llx observer=%s endpoint=%s helper=%s candidate=%s priority=%u single_peer=%d",
               state->socket->name, (unsigned long long)best.group, sc_public_key_hex(best.observer, (char[65]){0}),
               sockaddr_storage_to_str(&observer_address).str, sc_public_key_hex(best.helper, (char[65]){0}),
               sockaddr_storage_to_str(&helper_address).str, best.helper_priority, single_peer_pair(&best));
    return 0;
}

static void publish(struct NAT_DETECTION *detector) {
    if (detector->stopping || !detector->instance->topo_groups) return;
    detector->publish_pending = topo_node_update_my_addresses(detector->instance) < 0;
    if (detector->publish_pending) detector->publish_after = get_time_tb() + 10000;
    if (detector->publish_pending) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT profile publication failed; retry scheduled");
    else chat_event_post(detector->instance, CHAT_EVT_LOCAL_SOCKETS, NULL, 0);
}

static struct nat_socket *socket_state(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket, int create) {
    if (!detector || !socket || socket->probe_only || detector->stopping) return NULL;
    for (struct nat_socket *state = detector->sockets; state; state = state->next)
        if (state->socket == socket) return state;
    if (!create) return NULL;
    struct nat_socket *state = u_calloc(1, sizeof(*state));
    if (!state) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT endpoint state allocation failed socket=%s", socket->name); return NULL; }
    state->detector = detector; state->socket = socket; state->local = socket->local_addr;
    if (socket->is_tcp) state->local = socket->interface_addr;
    state->due = get_time_tb() + 2500;
    state->cold_until = state->due + NAT_ROUND_TB;
    state->next = detector->sockets; detector->sockets = state;
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT endpoint created socket=%s id=%u generation=%016llx protocol=%s local=%s",
                socket->name, socket->sock_id, (unsigned long long)socket->generation,
                socket->is_tcp ? "TCP" : "UDP", sockaddr_storage_to_str(&state->local).str);
    return state;
}

/* Адрес/поколение копируются; запрос удерживает только контекст owner, не сокеты. Worker — метка для callback. */
struct nat_history_context {
    struct UTUN_INSTANCE *instance;
    struct NAT_DETECTION *owner;
    struct db_async *worker;
    uint64_t generation;
    uint8_t socket_id, local[ROUTE_ADDR_WIRE_SIZE];
};
struct nat_history_write {
    struct nat_history_context context;
    unsigned count;
    int marker;
    uint8_t contacts[][17];
};
static int history_context(struct nat_socket *state, struct nat_history_context *context) {
    *context = (struct nat_history_context){.instance=state->detector->instance, .owner=state->detector,
        .worker=state->detector->instance->db_worker, .generation=state->socket->generation, .socket_id=state->socket->sock_id};
    return route_ping_addr_encode(context->local, &state->local);
}
static struct nat_socket *history_state(const struct nat_history_context *context) {
    struct NAT_DETECTION *detector = context->instance->nat_det;
    if (!detector || detector != context->owner || detector->stopping || context->instance->db_worker != context->worker) return NULL;
    for (struct nat_socket *state = detector->sockets; state; state = state->next) {
        uint8_t local[ROUTE_ADDR_WIRE_SIZE];
        if (state->socket->sock_id == context->socket_id && state->socket->generation == context->generation &&
            !route_ping_addr_encode(local, &state->local) && !memcmp(local, context->local, sizeof(local))) return state;
    }
    return NULL;
}
static void history_release(struct NAT_DETECTION *detector) {
    if (!detector->history_refs) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT history reference underflow"); return; }
    if (!--detector->history_refs) u_free(detector);
}
/* Контекст — первое поле write job; один cleanup подходит и для read, и для write. */
static void history_free(void *arg) {
    struct nat_history_context *context = arg; struct NAT_DETECTION *owner = context->owner;
    u_free(arg); history_release(owner);
}
/* Worker читает только скопированные local/count/contacts; вся пачка фиксируется одним WRITE commit. */
static int history_write_sql(sqlite3 *db, void *arg) {
    const struct nat_history_write *job = arg; sqlite3_stmt *st = NULL; unsigned row = 0;
    const char *stage = "prepare";
    int rc = sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO nat_contacts(local,remote) VALUES(?,?)", -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind local"; rc = sqlite3_bind_blob(st, 1, job->context.local, ROUTE_ADDR_WIRE_SIZE, SQLITE_STATIC); }
    for (; rc == SQLITE_OK && row < job->count; row++) {
        stage = "bind remote"; rc = sqlite3_bind_blob(st, 2, job->contacts[row], 17, SQLITE_STATIC);
        if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
        if (rc == SQLITE_DONE) { stage = "reset"; rc = sqlite3_reset(st); }
    }
    int final = sqlite3_finalize(st); if (rc == SQLITE_OK && final != SQLITE_OK) { stage = "finalize"; rc = final; }
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT history write failed stage=%s row=%u/%u rc=%d finalize=%d: %s",
        stage, row, job->count, rc, final, (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return rc;
}
static void history_write_done(void *arg, const struct db_async_result *result) {
    struct nat_history_write *job = arg; struct nat_socket *state = history_state(&job->context);
    if (!state) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT history result ignored for removed endpoint id=%u generation=%016llx",
            job->context.socket_id, (unsigned long long)job->context.generation); return;
    }
    if (job->marker) state->history_marker_pending = 0;
    if (result->status) {
        state->history_incomplete = 1; state->history_complete = 0; state->cycle_history_complete = 0; state->filter_clean = 0;
        state->history_retry_tb = get_time_tb() + NAT_RETRY_TB;
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT history persistence failed socket=%s rows=%u status=%d rc=%d marker_saved=%u: %s",
            state->socket->name, job->count, result->status, result->sqlite_code, state->history_marker_saved,
            result->message[0] ? result->message : sqlite3_errstr(result->sqlite_code));
    } else {
        if (job->marker) state->history_marker_saved = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT history write complete socket=%s rows=%u marker=%d", state->socket->name, job->count, job->marker);
    }
}
static int history_write(struct nat_socket *state, const uint8_t *contacts, unsigned count, int marker) {
    if (!count && !marker) return 0;
    struct nat_history_write *job = u_malloc(sizeof(*job) + (count + (marker != 0)) * 17);
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT history write allocation failed contacts=%u marker=%d", count, marker); return -1; }
    if (history_context(state, &job->context)) { u_free(job); return -1; }
    job->count = count + (marker != 0); job->marker = marker;
    if (count) memcpy(job->contacts, contacts, count * 17);
    if (marker) { memset(job->contacts[count], 0, 17); state->history_marker_pending = 1; }
    job->context.owner->history_refs++;
    struct UTUN_INSTANCE *inst = state->detector->instance;
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="nat-history", .mode=DB_ASYNC_WRITE,
            .work=history_write_sql, .data=job, .done=history_write_done, .cleanup=history_free}); return 0;
    }
    /* Временно единственный прежний писатель; RAM без worker нужен изолированным тестам. */
    sqlite3 *db = inst->topo_sqlite_db; const char *file = db ? sqlite3_db_filename(db, "main") : NULL;
    int code = db && (inst->db_worker || (file && !*file)) ? history_write_sql(db, job) : SQLITE_MISUSE;
    struct db_async_result result = {.status=code == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL, .sqlite_code=code};
    history_write_done(job, &result); history_free(job); return code == SQLITE_OK ? 0 : -1;
}
static void history_mark_incomplete(struct nat_socket *state) {
    state->history_complete = 0; state->history_incomplete = 1; state->cycle_history_complete = 0; state->filter_clean = 0;
    if (state->history_marker_saved || state->history_marker_pending || get_time_tb() < state->history_retry_tb) return;
    state->history_retry_tb = get_time_tb() + NAT_RETRY_TB;
    if (history_write(state, NULL, 0, 1)) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT incomplete history marker persistence failed; retry delayed");
}
static void history_contact(struct nat_socket *state, int type, const uint8_t *remote, size_t bytes) {
    static const uint8_t marker[17];
    if (type == SQLITE_BLOB && bytes == 17 && remote && !memcmp(remote, marker, 17)) {
        state->history_incomplete = 1; state->history_marker_saved = 1; return;
    }
    if (type != SQLITE_BLOB || bytes != 17 || !remote || (remote[0] != 4 && remote[0] != 6)) {
        state->history_incomplete = 1;
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT history corrupt contact socket=%s type=%d bytes=%zu", state->socket->name, type, bytes); return;
    }
    for (unsigned i = 0; i < state->contact_count; i++) if (!memcmp(state->contacts[i], remote, 17)) return;
    if (state->contact_count == NAT_MAX_CONTACTS) {
        if (!state->history_incomplete) DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT stored contact history exceeds limit socket=%s limit=%d",
            state->socket->name, NAT_MAX_CONTACTS);
        state->history_incomplete = 1; return;
    }
    memcpy(state->contacts[state->contact_count++], remote, 17);
}
static void history_loaded(struct nat_socket *state, unsigned pending) {
    state->history_loaded = 1; state->history_complete = !state->history_incomplete; state->history_retry_tb = 0;
    int marker = state->history_incomplete && !state->history_marker_saved && !state->history_marker_pending;
    /* Только контакты, накопленные до ответа; старые строки повторно не записываются. */
    if (history_write(state, &state->contacts[0][0], pending, marker)) history_mark_incomplete(state);
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT history loaded socket=%s contacts=%u complete=%u pending_saves=%u",
        state->socket->name, state->contact_count, state->history_complete, pending);
}
static void history_read_done(void *arg, const struct db_sql_result *result) {
    struct nat_history_context *context = arg; struct nat_socket *state = history_state(context);
    if (!state) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT history read ignored for removed endpoint id=%u generation=%016llx",
            context->socket_id, (unsigned long long)context->generation); history_free(context); return;
    }
    state->history_pending = 0;
    if (result->outcome.status || result->columns != 1) {
        state->history_complete = 0; state->history_retry_tb = get_time_tb() + NAT_RETRY_TB;
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT history read failed socket=%s status=%d rc=%d columns=%u; retry delayed: %s",
            state->socket->name, result->outcome.status, result->outcome.sqlite_code, result->columns, result->outcome.message);
    } else {
        unsigned pending = state->contact_count;
        for (size_t i = 0; i < result->rows; i++) {
            const struct db_value *value = &result->values[i]; history_contact(state, value->type, value->data, value->bytes);
        }
        history_loaded(state, pending);
    }
    history_free(context);
}
static void history_load(struct nat_socket *state) {
    if (state->socket->is_tcp || state->history_pending || get_time_tb() < state->history_retry_tb) return;
    if (state->history_loaded) { if (state->history_incomplete) history_mark_incomplete(state); return; }
    struct UTUN_INSTANCE *inst = state->detector->instance; struct nat_history_context context;
    if (history_context(state, &context)) return;
    const char *sql = "SELECT remote FROM nat_contacts WHERE local=? ORDER BY remote LIMIT ?";
    if (inst->db_worker) {
        struct nat_history_context *request = u_malloc(sizeof(*request));
        if (!request) {
            state->history_complete = 0; state->history_retry_tb = get_time_tb() + NAT_RETRY_TB;
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT history read allocation failed socket=%s; retry delayed", state->socket->name); return;
        }
        *request = context; request->owner->history_refs++; state->history_pending = 1; state->history_complete = 0;
        db_query(inst->db_worker, sql, DB_ARGS(DB_BLOB(context.local, sizeof(context.local)), DB_I64(NAT_MAX_CONTACTS + 1)),
            history_read_done, request); return;
    }
    /* Только RAM: файловое чтение без worker запрещено. */
    sqlite3 *db = inst->topo_sqlite_db; const char *file = db ? sqlite3_db_filename(db, "main") : NULL;
    state->history_retry_tb = get_time_tb() + NAT_RETRY_TB;
    if (!file || *file) {
        state->history_complete = 0; DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT history has no asynchronous reader socket=%s", state->socket->name); return;
    }
    sqlite3_stmt *st = NULL; unsigned pending = state->contact_count;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, context.local, sizeof(context.local), SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 2, NAT_MAX_CONTACTS + 1);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int type = sqlite3_column_type(st, 0), bytes = sqlite3_column_bytes(st, 0); const uint8_t *remote = sqlite3_column_blob(st, 0);
        if (type == SQLITE_BLOB && bytes && !remote) { rc = SQLITE_NOMEM; break; }
        history_contact(state, type, remote, (size_t)bytes);
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        state->history_complete = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT RAM history read failed rc=%d finalize=%d socket=%s", rc, final, state->socket->name); return;
    }
    history_loaded(state, pending);
}

static int contacted(struct nat_socket *state, const struct sockaddr_storage *address) {
    uint8_t remote[ROUTE_ADDR_WIRE_SIZE];
    if (!state->history_complete || route_ping_addr_encode(remote, address)) return 1;
    for (unsigned i = 0; i < state->contact_count; i++) if (!memcmp(state->contacts[i], remote, 17)) return 1;
    return 0;
}

void nat_detection_note_send(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
                              const struct sockaddr_storage *destination) {
    if (!detector || !socket || socket->is_tcp || socket->probe_only || socket->socks_udp || detector->stopping) return;
    struct nat_socket *state = socket_state(detector, socket, 1);
    if (!state) return;
    history_load(state);
    if ((state->phase == NAT_PREPARE_C || state->phase == NAT_PRE || state->phase == NAT_PRE_WAIT) && !state->pre_received &&
        route_ping_addr_equal(destination, &state->helper_address, 0)) {
        if (state->filter_clean) DEBUG_WARN(DEBUG_CATEGORY_NAT,
            "NAT ingress contaminated by outgoing UDP socket=%s round=%016llx destination=%s",
            socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(destination).str);
        state->filter_clean = 0;
    }
    uint8_t local[ROUTE_ADDR_WIRE_SIZE], remote[ROUTE_ADDR_WIRE_SIZE];
    if (route_ping_addr_encode(local, &state->local) || route_ping_addr_encode(remote, destination)) return;
    for (unsigned i = 0; i < state->contact_count; i++) if (!memcmp(state->contacts[i], remote, 17)) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT first UDP contact node=%s socket=%s id=%u generation=%016llx phase=%u group=%016llx"
                " round=%016llx destination=%s reserved=%u cold_remaining_tb=%llu",
                sc_public_key_hex(detector->instance->my_ed25519_pubkey, (char[65]){0}), socket->name, socket->sock_id,
                (unsigned long long)socket->generation, state->phase, (unsigned long long)state->group_id,
                (unsigned long long)state->round, sockaddr_storage_to_str(destination).str,
                state->reservation_fingerprint != 0,
                (unsigned long long)(state->cold_until > get_time_tb() ? state->cold_until - get_time_tb() : 0));
    if (state->contact_count == NAT_MAX_CONTACTS) {
        if (state->history_complete) DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT contact history full socket=%s", socket->name);
        if (!state->history_incomplete) history_mark_incomplete(state);
        return;
    }
    memcpy(state->contacts[state->contact_count++], remote, 17);
    if (!state->history_loaded) { state->history_complete = 0; return; }
    if (history_write(state, remote, 1, 0)) history_mark_incomplete(state);
}

static int send_control(struct NAT_DETECTION *detector, uint64_t group_id, const uint8_t destination[32], struct nat_wire *message) {
    if (!control_route(detector, group_id, destination)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT control route unavailable group=%016llx peer=%s",
                   (unsigned long long)group_id, sc_public_key_hex(destination, (char[65]){0})); return -1;
    }
    struct ll_entry *entry = queue_entry_new(0);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT control queue allocation failed"); return -1; }
    entry->dgram = u_malloc(1 + sizeof(*message));
    if (!entry->dgram) { queue_entry_free(entry); DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT control allocation failed"); return -1; }
    message->cmd = ETCP_RT_ID_NAT_DETECTION; message->version = NAT_WIRE_VERSION;
    entry->dgram[0] = ETCP_RT_ID_NAT_DETECTION;
    memcpy(entry->dgram + 1, message, sizeof(*message)); entry->len = 1 + sizeof(*message);
    if (etcp_route_send_key(detector->instance, group_id, destination, entry, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT control send failed group=%016llx peer=%s command=%u",
                    (unsigned long long)group_id, sc_public_key_hex(destination, (char[65]){0}), message->command); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT control TX group=%016llx command=%u step=%u peer=%s owner=%s round=%016llx helper=%s",
                (unsigned long long)group_id, message->command, message->step, sc_public_key_hex(destination, (char[65]){0}),
                sc_public_key_hex(message->owner, (char[65]){0}), (unsigned long long)read64(message->round),
                sc_public_key_hex(message->helper, (char[65]){0}));
    return 0;
}

/* Один бюджет ожидания для UDP-пробы, серий помощника и паузы доставки последнего пакета. */
static int probe_timeout_ms(struct nat_socket *state) {
    int timeout = 1000;
    struct TOPO_GROUP *group = topo_groups_find(state->detector->instance->topo_groups, state->group_id);
    struct TOPO_NODEPATH *path = topo_node_best_path(topo_node_find_by_ed(group, state->current_pair.observer));
    struct ETCP_CONN *connection = path ? path->conn : NULL;
    for (struct ETCP_LINK *link = connection ? connection->links : NULL; link; link = link->next)
        if (link->rtt_last / 10 * 4 > timeout) timeout = link->rtt_last / 10 * 4;
    return timeout;
}

static int make_message(struct nat_socket *state, struct nat_wire *message, uint8_t command) {
    memset(message, 0, sizeof(*message)); message->command = command;
    message->owner_socket = state->socket->sock_id; message->helper_socket = state->helper_socket;
    memcpy(message->owner, state->detector->instance->my_ed25519_pubkey, 32);
    write64(message->generation, state->socket->generation); write64(message->round, state->round);
    memcpy(message->helper, state->current_pair.helper, 32); memcpy(message->observer, state->current_pair.observer, 32); write64(message->token, state->token);
    if (route_ping_addr_encode(message->target, &state->baseline)) return -1;
    if (state->helper_address.ss_family && route_ping_addr_encode(message->primary, &state->helper_address)) return -1;
    if (state->observer_address.ss_family && route_ping_addr_encode(message->observer_address, &state->observer_address)) return -1;
    int timeout = probe_timeout_ms(state);
    message->timeout_ms[0] = timeout >> 8; message->timeout_ms[1] = timeout;
    return 0;
}

/* UDP-запрос коррелируется отдельным token; результат придёт через защищённый router, не через PONG. */
static int observe_control(struct ETCP_SOCKET *socket, const uint8_t key[32], const struct sockaddr_storage *target,
                           uint64_t group, uint64_t round, uint8_t step, uint64_t token, int timeout,
                           etcp_ping_callback_t callback, void *arg) {
    uint8_t tag[NAT_QUERY_TAG_SIZE];
    if (route_ping_make_tag(tag, socket, group, round)) return -1;
    tag[ROUTE_OBSERVE_TAG_SIZE] = step; write64(tag + ROUTE_OBSERVE_TAG_SIZE + 1, token);
    return etcp_send_ping_to_socket(socket->instance, socket, key, target, timeout, callback, arg, tag, sizeof(tag),
                                    ETCP_PING_FLAG_OBSERVE | ETCP_PING_FLAG_NO_REPLY);
}

static struct nat_binding *binding_find(struct NAT_DETECTION *detector, uint64_t group_id, const uint8_t owner[32],
                                        uint64_t generation, uint64_t round) {
    for (struct nat_binding *binding = detector->bindings; binding; binding = binding->next)
        if (binding->group_id == group_id && !memcmp(binding->owner, owner, 32) && binding->generation == generation && binding->round == round &&
            binding->expires > get_time_tb()) return binding;
    return NULL;
}

void nat_detection_record_observation(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
                                      const uint8_t peer_key[32], const struct sockaddr_storage *source,
                                      const uint8_t *tag, size_t length) {
    if (!detector || detector->stopping) return;
    if (!socket || socket->socks_udp || (length != ROUTE_OBSERVE_TAG_SIZE && length != NAT_QUERY_TAG_SIZE) ||
        !tag || memcmp(tag, "NAT4UDP!", 8)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT observation query has invalid tag length=%zu", length); return;
    }
    uint64_t generation = read64(tag + 9), round = read64(tag + 17);
    struct TOPO_GROUP *group = topo_groups_find_wire(detector->instance->topo_groups, tag + ROUTE_OBSERVE_GROUP_OFF);
    uint64_t group_id = group ? group->group_id : 0;
    const struct TOPO_NODE *node = group ? group_node_x(detector, group_id, peer_key) : NULL;
    if (!generation || !round || !node) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT observation has invalid group/identity group=%016llx transport_key=%s",
                   (unsigned long long)group_id, sc_public_key_hex(peer_key, (char[65]){0})); return;
    }
    char owner[65]; sc_public_key_hex(node->ed25519_public_key, owner);
    int eligible = public_address(detector, source);
    if (!eligible) DEBUG_WARN(DEBUG_CATEGORY_NAT,
        "NAT observation rejected by receiver group=%016llx owner=%s round=%016llx source=%s reason=non_public_source",
        (unsigned long long)group_id, owner, (unsigned long long)round, sockaddr_storage_to_str(source).str);
    if (length == NAT_QUERY_TAG_SIZE) {
        struct nat_wire report = {.command = NAT_OBSERVATION, .owner_socket = tag[8], .step = tag[ROUTE_OBSERVE_TAG_SIZE],
            .reason = eligible ? NAT_REASON_NONE : NAT_REASON_INVALID_OBSERVATION};
        memcpy(report.owner, node->ed25519_public_key, 32); write64(report.generation, generation); write64(report.round, round);
        write64(report.token, read64(tag + ROUTE_OBSERVE_TAG_SIZE + 1));
        memcpy(report.observer, detector->instance->my_ed25519_pubkey, 32);
        memcpy(report.helper, report.observer, 32);
        int receiver_error = direct_udp_socket(detector, socket) && route_ping_addr_encode(report.observer_address, &socket->local_addr);
        if (receiver_error || route_ping_addr_encode(report.target, source) || send_control(detector, group_id, report.owner, &report))
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT routed observation delivery failed group=%016llx owner=%s round=%016llx",
                       (unsigned long long)group_id, owner, (unsigned long long)round);
    }
    /* Сокет C того же пира может видеть другой mapping. Делегирование остаётся привязанным к B. */
    if (!eligible || (length == NAT_QUERY_TAG_SIZE && tag[ROUTE_OBSERVE_TAG_SIZE] != NAT_OBSERVE_B &&
                      tag[ROUTE_OBSERVE_TAG_SIZE] != NAT_REPEAT_B)) return;
    struct nat_binding *binding = binding_find(detector, group_id, node->ed25519_public_key, generation, round);
    if (!binding) {
        unsigned count = 0;
        for (struct nat_binding *item = detector->bindings; item; item = item->next) count++;
        if (count >= NAT_MAX_BINDINGS) { DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT observation cache full"); return; }
        binding = u_calloc(1, sizeof(*binding));
        if (!binding) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT observation cache allocation failed"); return; }
        binding->next = detector->bindings; detector->bindings = binding;
    }
    binding->group_id = group_id; memcpy(binding->owner, node->ed25519_public_key, 32); binding->generation = generation;
    binding->round = round; binding->owner_socket = tag[8];
    binding->observer_socket = socket->sock_id; binding->observer_generation = socket->generation;
    binding->target = *source; binding->expires = get_time_tb() + NAT_ROUND_TB;
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT observation group=%016llx owner=%s socket=%u generation=%016llx round=%016llx observed=%s via=%s",
                (unsigned long long)group_id, owner, tag[8], (unsigned long long)generation, (unsigned long long)round,
                sockaddr_storage_to_str(source).str, socket->name);
}

static void clear_profile(struct nat_socket *state, uint8_t reason) {
    int changed = state->socket->address_profile.mapping || state->socket->address_profile.inbound ||
        state->socket->nat_addr.ss_family;
    memset(&state->socket->address_profile, 0, sizeof(state->socket->address_profile));
    memset(&state->socket->nat_addr, 0, sizeof(state->socket->nat_addr));
    state->completed_tb = 0;
    state->cold_until = get_time_tb() + NAT_ROUND_TB + 2500;
    memset(&state->measured_profile, 0, sizeof(state->measured_profile));
    memset(&state->measured_address, 0, sizeof(state->measured_address));
    memset(&state->observed_address, 0, sizeof(state->observed_address));
    if (changed) {
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT profile invalidated socket=%s generation=%016llx reason=%s",
                   state->socket->name, (unsigned long long)state->socket->generation, nat_check_reason_name(reason));
        publish(state->detector);
    }
}

/* Измерение рабочего endpoint приоритетно; STUN того же пути даёт только предварительный EIM?. */
static void select_profile(struct nat_socket *state) {
    const struct address_profile *current = &state->socket->address_profile;
    struct address_profile selected = state->measured_profile;
    struct sockaddr_storage selected_address = {0};
    if (state->socket->is_tcp) {
        selected_address = state->observed_address;
        if (!selected.source && !public_address(state->detector, &state->local)) selected.source = ADDRESS_SOURCE_ETCP;
    }
    if (state->measured_profile.source == ADDRESS_SOURCE_ETCP) {
        selected = state->measured_profile;
        if (selected.mapping == ADDRESS_MAP_EIM) selected_address = state->measured_address;
    }
    if (!selected.mapping && state->observed_address.ss_family) {
        selected.source = ADDRESS_SOURCE_ETCP; selected_address = state->observed_address;
    }
    struct stun_nat_result stun;
    uint64_t now = get_time_tb();
    if (!selected.mapping && !stun_nat_get_socket_result(state->detector->instance, state->socket, &stun) &&
        (stun.status == STUN_NAT_COMPLETE || stun.status == STUN_NAT_PARTIAL) &&
        stun.measured_at_tb && now >= stun.measured_at_tb && now - stun.measured_at_tb < STUN_NAT_TTL_TB &&
        stun.server_count == 2 && stun.translated == 1 && (stun.mapping == STUN_MAP_MATCH || stun.mapping == STUN_MAP_EIM) &&
        public_address(state->detector, &stun.servers[0]) && public_address(state->detector, &stun.servers[1]) &&
        public_address(state->detector, &stun.mapped[0]) &&
        (!selected_address.ss_family || route_ping_addr_equal(&selected_address, &stun.mapped[0], 0))) {
        selected.mapping = ADDRESS_MAP_EIM; selected.inbound = ADDRESS_IN_UNKNOWN; selected.source = ADDRESS_SOURCE_STUN;
    }
    int address_changed = selected_address.ss_family != state->socket->nat_addr.ss_family ||
        (selected_address.ss_family && !route_ping_addr_equal(&selected_address, &state->socket->nat_addr, 1));
    if (selected.mapping == current->mapping && selected.inbound == current->inbound &&
        selected.source == current->source && !address_changed) return;
    if (selected.source == ADDRESS_SOURCE_STUN)
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT tentative EIM? socket=%s STUN_local=%s netif=%u evidence=%016llx"
                   " mapping=%s filtering=%s age_tb=%llu working_address=%s",
                   state->socket->name, sockaddr_storage_to_str(&stun.local).str, stun.key.netif_index,
                   (unsigned long long)stun.evidence_id, stun_nat_mapping_name(stun.mapping),
                   stun_nat_filtering_name(stun.filtering), (unsigned long long)(now - stun.measured_at_tb),
                   sockaddr_storage_to_str(&selected_address).str);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT selected socket=%s generation=%016llx source=%s previous=%s mapping=%s inbound=%s"
               " address_changed=%d reason=%s",
               state->socket->name, (unsigned long long)state->socket->generation, address_profile_source_name(selected.source),
               address_profile_source_name(current->source), address_mapping_name(selected.mapping), address_inbound_name(selected.inbound),
               address_changed, nat_check_reason_name(state->reason));
    state->socket->address_profile = selected;
    state->socket->nat_addr = selected_address;
    publish(state->detector);
}

/* Одно наблюдение подтверждает NAT-адрес, но не его независимость от IP назначения. */
static void remember_address(struct nat_socket *state, const struct sockaddr_storage *observed) {
    if (route_ping_addr_equal(&state->local, observed, 1)) return;
    if (route_ping_addr_equal(&state->observed_address, observed, 1)) return;
    state->observed_address = *observed;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT address observed socket=%s local=%s external=%s mapping=%s scope=one_destination",
               state->socket->name, sockaddr_storage_to_str(&state->local).str, sockaddr_storage_to_str(observed).str,
               address_mapping_name(state->measured_profile.mapping));
    select_profile(state);
}

static void owner_finish(struct nat_socket *state, uint8_t reason) {
    if (state->phase && reason != NAT_REASON_CANCELLED &&
        !participants_allowed(state->detector, state->group_id, state->current_pair.observer, state->current_pair.helper)) reason = NAT_REASON_CANCELLED;
    if (reason == NAT_REASON_BUDGET && state->pair_full_budget) {
        reason = NAT_REASON_TIMEOUT;
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT pair full budget exhausted socket=%s round=%016llx; advancing candidate",
                   state->socket->name, (unsigned long long)state->round);
    }
    /* Неудача входящего испытания не мешает сравнить mapping через уже известный публичный endpoint C. */
    if ((reason == NAT_REASON_TIMEOUT || reason == NAT_REASON_CONTROL_LOST || reason == NAT_REASON_SEND_ERROR ||
         reason == NAT_REASON_CONTACT_HISTORY) &&
        (state->phase == NAT_PREPARE_C || state->phase == NAT_PRE || state->phase == NAT_POST) &&
        get_time_tb() < state->cycle_deadline && public_address(state->detector, &state->helper_address) &&
        !route_ping_addr_equal(&state->helper_address, &state->observer_address, 0) &&
        !route_ping_addr_equal(&state->helper_address, &state->baseline, 0)) {
        if (reason == NAT_REASON_CONTACT_HISTORY || !state->pre_received) state->filter_clean = 0;
        state->phase = state->helper_seen.ss_family ? NAT_REPEAT_B : NAT_OBSERVE_C; state->retries = 0;
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT mapping continues without ingress result socket=%s round=%016llx helper=%s reason=%s",
                   state->socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(&state->helper_address).str,
                   nat_check_reason_name(reason));
        if (owner_observe(state)) owner_finish(state, NAT_REASON_SEND_ERROR);
        return;
    }
    uint64_t now = get_time_tb();
    int next_pair = reason && state->phase && state->cycling &&
        reason != NAT_REASON_CANCELLED && reason != NAT_REASON_ADDRESS_CHANGED;
    etcp_cancel_pings(state->detector->instance, owner_ping_cb, state);
    etcp_cancel_pings(state->detector->instance, owner_heartbeat_cb, state);
    int pause = next_pair && now >= state->cycle_deadline;
    if (next_pair && reason != NAT_REASON_BUDGET) state->cursor = state->current_pair;
    state->active_ping = 0; state->heartbeat_active = 0; state->phase = NAT_IDLE; state->reason = reason;
    if (next_pair) {
        state->forced = 1; state->due = now + (pause ? NAT_RETRY_TB : 1000);
        if (reason != NAT_REASON_BUDGET) state->helper_reselections++;
        if (pause) DEBUG_INFO(DEBUG_CATEGORY_NAT,
            "NAT candidate cycle paused socket=%s round=%016llx retry_current=%d resume_in_tb=%llu mapping=%s inbound=%s",
            state->socket->name, (unsigned long long)state->round, reason == NAT_REASON_BUDGET,
            (unsigned long long)NAT_RETRY_TB, address_mapping_name(state->measured_profile.mapping),
            address_inbound_name(state->measured_profile.inbound));
    } else {
        state->cycling = 0; memset(state->preferred_observer, 0, 32); state->cursor = (struct nat_pair){0};
        state->current_pair = (struct nat_pair){0};
        int complete = state->measured_profile.mapping == ADDRESS_MAP_DEPENDENT ||
            (state->measured_profile.mapping && state->measured_profile.inbound);
        state->due = get_time_tb() + (reason == NAT_REASON_ADDRESS_CHANGED ? 2500 : complete ? NAT_REFRESH_TB : NAT_RETRY_TB);
    }
    if (reason) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT round incomplete socket=%s group=%016llx generation=%016llx round=%016llx"
                   " observer_endpoint=%s helper_endpoint=%s reason=%s next_pair=%d",
                   state->socket->name, (unsigned long long)state->group_id, (unsigned long long)state->socket->generation,
                   (unsigned long long)state->round, sockaddr_storage_to_str(&state->observer_address).str,
                   sockaddr_storage_to_str(&state->helper_address).str, nat_check_reason_name(reason), next_pair);
    }
    select_profile(state);
}

static void owner_apply(struct nat_socket *state, uint8_t mapping, uint8_t inbound) {
    struct address_profile profile = {.mapping = mapping, .inbound = inbound, .source = ADDRESS_SOURCE_ETCP};
    uint8_t partial_reason = inbound != ADDRESS_IN_UNKNOWN ? NAT_REASON_NONE : !state->filter_clean ? NAT_REASON_CONTACT_HISTORY :
        mapping == ADDRESS_MAP_DEPENDENT ? NAT_REASON_DEPENDENT_MAPPING : NAT_REASON_TIMEOUT;
    if (inbound == ADDRESS_IN_UNKNOWN && state->measured_profile.source == ADDRESS_SOURCE_ETCP &&
        mapping == state->measured_profile.mapping && route_ping_addr_equal(&state->measured_address, &state->baseline, 1)) {
        profile.inbound = state->measured_profile.inbound;
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT incomplete ingress preserves previous result socket=%s round=%016llx inbound=%s reason=%s",
                    state->socket->name, (unsigned long long)state->round, address_inbound_name(profile.inbound),
                    nat_check_reason_name(partial_reason));
    }
    state->measured_profile = profile; state->measured_address = state->baseline;
    state->cycle_mapping_seen = 1;
    state->completed_tb = get_time_tb();
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT profile socket=%s id=%u generation=%016llx round=%016llx mapping=%s inbound=%s effective_inbound=%s"
               " local=%s external=%s observer=%s observer_endpoint=%s helper=%s helper_endpoint=%s helper_external=%s clean=%u partial_reason=%s",
               state->socket->name, state->socket->sock_id, (unsigned long long)state->socket->generation,
               (unsigned long long)state->round, address_mapping_name(mapping), address_inbound_name(inbound),
               address_inbound_name(profile.inbound),
               sockaddr_storage_to_str(&state->local).str, sockaddr_storage_to_str(&state->baseline).str,
               sc_public_key_hex(state->current_pair.observer, (char[65]){0}), sockaddr_storage_to_str(&state->observer_address).str,
               sc_public_key_hex(state->current_pair.helper, (char[65]){0}), sockaddr_storage_to_str(&state->helper_address).str,
               sockaddr_storage_to_str(&state->helper_seen).str, state->filter_clean,
               nat_check_reason_name(partial_reason));
    int complete = inbound != ADDRESS_IN_UNKNOWN || mapping == ADDRESS_MAP_DEPENDENT;
    owner_finish(state, complete ? NAT_REASON_NONE : partial_reason);
    if (complete) state->reason = partial_reason;
}

static int owner_run(struct nat_socket *state, uint8_t step) {
    if (random_id(&state->token)) return -1;
    struct nat_wire message;
    if (make_message(state, &message, NAT_RUN)) return -1;
    message.step = step;
    state->phase = step == NAT_STEP_PREPARE ? NAT_PREPARE_C :
                   step == NAT_STEP_PRE ? NAT_PRE : NAT_POST;
    uint64_t budget = (uint64_t)probe_timeout_ms(state) * 10 *
        (step == NAT_STEP_PREPARE ? 2 * NAT_PROBE_COUNT + 2 : NAT_PROBE_COUNT + 2);
    state->deadline = get_time_tb() + budget;
    if (state->deadline > state->cycle_deadline) state->deadline = state->cycle_deadline;
    if (step == NAT_STEP_PRE) state->pre_started = get_time_tb();
    return send_control(state->detector, state->group_id, state->current_pair.observer, &message);
}

static int owner_observe(struct nat_socket *state) {
    if (!participants_allowed(state->detector, state->group_id, state->current_pair.observer, state->current_pair.helper)) return -1;
    const struct sockaddr_storage *target = &state->observer_address;
    const uint8_t *key = state->observer_key;
    if (state->phase == NAT_OBSERVE_C || state->phase == NAT_REPEAT_C) { target = &state->helper_address; key = state->helper_key; }
    int timeout = probe_timeout_ms(state);
    state->deadline = state->cycle_deadline;
    if (state->phase == NAT_OBSERVE_C && !state->retries) {
        state->contact_cutoff = state->contact_count;
        if (contacted(state, &state->helper_address)) state->filter_clean = 0;
    }
    if (random_id(&state->observation_token)) return -1;
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT observe TX socket=%s phase=%u round=%016llx token=%016llx target=%s retries=%u",
                state->socket->name, state->phase, (unsigned long long)state->round,
                (unsigned long long)state->observation_token, sockaddr_storage_to_str(target).str, state->retries);
    if (observe_control(state->socket, key, target, state->group_id, state->round, state->phase,
                        state->observation_token, timeout, owner_ping_cb, state)) return -1;
    state->active_ping = 1; return 0;
}

/* Прямой контроль к B поддерживает mapping и позволяет отличать входящее испытание от общей недоступности UDP. */
static void owner_heartbeat_cb(int success, uint16_t rtt, void *arg, uint64_t nonce, const uint8_t *data, size_t length) {
    (void)rtt; (void)nonce;
    struct nat_socket *state = arg; state->heartbeat_active = 0;
    if (!state->phase) return;
    if (!participants_allowed(state->detector, state->group_id, state->current_pair.observer, state->current_pair.helper)) {
        owner_finish(state, NAT_REASON_CANCELLED); return;
    }
    if (get_time_tb() >= state->cycle_deadline) { owner_finish(state, NAT_REASON_BUDGET); return; }
    struct sockaddr_storage observed;
    if (success && !route_ping_observation(data, length, state->socket, state->group_id, state->round, &observed) &&
        public_address(state->detector, &observed)) {
        if (state->baseline.ss_family && !route_ping_addr_equal(&state->baseline, &observed, 1)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT anchor mapping changed socket=%s round=%016llx previous=%s current=%s",
                       state->socket->name, (unsigned long long)state->round,
                       sockaddr_storage_to_str(&state->baseline).str, sockaddr_storage_to_str(&observed).str);
            clear_profile(state, NAT_REASON_ADDRESS_CHANGED); owner_finish(state, NAT_REASON_ADDRESS_CHANGED); return;
        }
        state->heartbeat_received = get_time_tb();
        DEBUG_TRACE(DEBUG_CATEGORY_NAT, "NAT anchor healthy socket=%s round=%016llx observed=%s",
                    state->socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(&observed).str);
    } else DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT anchor direct response unavailable socket=%s round=%016llx",
                       state->socket->name, (unsigned long long)state->round);
    state->heartbeat_due = get_time_tb() + 10000;
}

static void owner_ping_cb(int success, uint16_t rtt, void *arg, uint64_t nonce, const uint8_t *data, size_t length) {
    (void)rtt; (void)nonce;
    struct nat_socket *state = arg;
    state->active_ping = 0;
    if (state->phase == NAT_IDLE) return;
    if (!participants_allowed(state->detector, state->group_id, state->current_pair.observer, state->current_pair.helper)) {
        owner_finish(state, NAT_REASON_CANCELLED); return;
    }
    if (get_time_tb() >= state->cycle_deadline) { owner_finish(state, NAT_REASON_BUDGET); return; }
    struct sockaddr_storage observed;
    if (!success || route_ping_observation(data, length, state->socket, state->group_id, state->round, &observed)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT observation incomplete socket=%s phase=%u round=%016llx attempt=%u reason=%s",
                   state->socket->name, state->phase, (unsigned long long)state->round, state->retries + 1,
                   success ? "invalid_observation" : "routed_report_timeout");
        if (++state->retries < NAT_PROBE_COUNT) { state->due = get_time_tb() + 1000; return; }
        owner_finish(state, success ? NAT_REASON_INVALID_OBSERVATION : NAT_REASON_TIMEOUT); return;
    }
    if (!public_address(state->detector, &observed)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT observation unusable socket=%s phase=%u round=%016llx observed=%s reason=non_public_source",
                   state->socket->name, state->phase, (unsigned long long)state->round, sockaddr_storage_to_str(&observed).str);
        owner_finish(state, NAT_REASON_INVALID_OBSERVATION); return;
    }
    state->retries = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT own observation socket=%s phase=%u round=%016llx observed=%s",
                state->socket->name, state->phase, (unsigned long long)state->round, sockaddr_storage_to_str(&observed).str);
    if (state->phase == NAT_OBSERVE_B) {
        state->baseline = observed;
        if ((state->measured_profile.mapping == ADDRESS_MAP_REAL_IP && !route_ping_addr_equal(&state->local, &observed, 1)) ||
            (state->measured_profile.mapping == ADDRESS_MAP_EIM &&
             !route_ping_addr_equal(&state->measured_address, &observed, 1)))
            clear_profile(state, NAT_REASON_ADDRESS_CHANGED);
        remember_address(state, &observed);
        if (key_empty(state->current_pair.helper)) { state->cycling = 0; owner_finish(state, NAT_REASON_NO_HELPER); return; }
        if (single_peer_pair(&state->current_pair)) {
            if (route_ping_addr_equal(&state->helper_address, &observed, 0)) {
                DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT alternate address equals own external IP socket=%s round=%016llx address=%s",
                           state->socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(&observed).str);
                owner_finish(state, NAT_REASON_NO_HELPER); return;
            }
            state->filter_clean = !contacted(state, &state->helper_address);
            if (state->filter_clean) {
                if (owner_run(state, NAT_STEP_PRE)) owner_finish(state, NAT_REASON_CONTROL_LOST);
            } else {
                DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT alternate address already contacted socket=%s round=%016llx helper=%s",
                            state->socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(&state->helper_address).str);
                state->phase = NAT_OBSERVE_C;
                if (owner_observe(state)) owner_finish(state, NAT_REASON_SEND_ERROR);
            }
            return;
        }
        if (owner_run(state, NAT_STEP_PREPARE)) owner_finish(state, NAT_REASON_CONTROL_LOST);
        return;
    }
    if (state->phase == NAT_OBSERVE_C) {
        state->helper_seen = observed;
        if (route_ping_addr_equal(&state->baseline, &observed, 1) && state->filter_clean &&
            state->pre_healthy && !state->pre_received) {
            if (owner_run(state, NAT_STEP_POST)) owner_finish(state, NAT_REASON_CONTROL_LOST);
            return;
        }
        state->phase = NAT_REPEAT_B;
    } else if (state->phase == NAT_REPEAT_B) {
        if (!route_ping_addr_equal(&observed, &state->baseline, 1)) {
            clear_profile(state, NAT_REASON_ADDRESS_CHANGED); owner_finish(state, NAT_REASON_ADDRESS_CHANGED); return;
        }
        state->phase = NAT_REPEAT_C;
    } else if (state->phase == NAT_REPEAT_C) {
        if (!route_ping_addr_equal(&observed, &state->helper_seen, 1)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper-direction mapping unstable socket=%s round=%016llx previous=%s current=%s",
                       state->socket->name, (unsigned long long)state->round,
                       sockaddr_storage_to_str(&state->helper_seen).str, sockaddr_storage_to_str(&observed).str);
            clear_profile(state, NAT_REASON_ADDRESS_CHANGED); owner_finish(state, NAT_REASON_ADDRESS_CHANGED); return;
        }
        int same = route_ping_addr_equal(&state->baseline, &state->helper_seen, 1);
        uint8_t mapping = same ? (route_ping_addr_equal(&state->local, &state->baseline, 1) ?
                                  ADDRESS_MAP_REAL_IP : ADDRESS_MAP_EIM) : ADDRESS_MAP_DEPENDENT;
        uint8_t inbound = state->filter_clean && state->pre_received ? ADDRESS_IN_OPEN :
            state->filter_clean && state->pre_healthy && state->post_received && same &&
            state->heartbeat_received >= state->pre_started &&
            get_time_tb() - state->heartbeat_received <= (uint64_t)probe_timeout_ms(state) * 20 ? ADDRESS_IN_CONTACT : ADDRESS_IN_UNKNOWN;
        owner_apply(state, mapping, inbound); return;
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT observation in unexpected phase=%u", state->phase);
        owner_finish(state, NAT_REASON_INVALID_OBSERVATION); return;
    }
    if (owner_observe(state)) owner_finish(state, NAT_REASON_SEND_ERROR);
}

void nat_detection_receive_ingress(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
                                   const uint8_t peer_key[32], const struct sockaddr_storage *source,
                                   const uint8_t *tag, size_t length) {
    struct nat_socket *state = socket_state(detector, socket, 0);
    if (!state || !tag || length != NAT_QUERY_TAG_SIZE) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT ingress ignored without pending endpoint/valid tag length=%zu", length); return;
    }
    uint8_t expected[ROUTE_OBSERVE_TAG_SIZE];
    if (route_ping_make_tag(expected, socket, state->group_id, state->round)) return;
    uint8_t step = state->phase == NAT_PREPARE_C ? NAT_STEP_PREPARE :
                   (state->phase == NAT_PRE || state->phase == NAT_PRE_WAIT) ? NAT_STEP_PRE :
                   (state->phase == NAT_POST || state->phase == NAT_POST_WAIT) ? NAT_STEP_POST : 0;
    if (!step || memcmp(tag, expected, sizeof(expected)) || tag[ROUTE_OBSERVE_TAG_SIZE] != step ||
        read64(tag + ROUTE_OBSERVE_TAG_SIZE + 1) != state->token ||
        memcmp(peer_key, state->helper_key, 32) ||
        source->ss_family != state->local.ss_family) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT ingress correlation/source mismatch socket=%s phase=%u round=%016llx source=%s",
                   socket->name, state->phase, (unsigned long long)state->round, sockaddr_storage_to_str(source).str); return;
    }
    if (!participants_allowed(detector, state->group_id, state->current_pair.observer, state->current_pair.helper)) {
        owner_finish(state, NAT_REASON_CANCELLED); return;
    }
    if (get_time_tb() >= state->cycle_deadline) { owner_finish(state, NAT_REASON_BUDGET); return; }
    if (single_peer_pair(&state->current_pair) && !route_ping_addr_equal(source, &state->helper_address, 1)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT alternate ingress source differs socket=%s round=%016llx source=%s expected=%s",
                   socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(source).str,
                   sockaddr_storage_to_str(&state->helper_address).str);
        owner_finish(state, NAT_REASON_NO_HELPER); return;
    }
    if (!public_address(detector, source)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper rejected by receiver socket=%s round=%016llx helper=%s source=%s reason=non_public_source",
                   socket->name, (unsigned long long)state->round, sc_public_key_hex(state->current_pair.helper, (char[65]){0}), sockaddr_storage_to_str(source).str);
        owner_finish(state, NAT_REASON_NO_HELPER); return;
    }
    if ((step == NAT_STEP_POST || state->pre_received) && !route_ping_addr_equal(source, &state->helper_address, 1)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT ingress endpoint mismatch socket=%s phase=%u round=%016llx source=%s expected=%s",
                   socket->name, state->phase, (unsigned long long)state->round,
                   sockaddr_storage_to_str(source).str, sockaddr_storage_to_str(&state->helper_address).str); return;
    }
    if (route_ping_addr_equal(source, &state->observer_address, 0) || route_ping_addr_equal(source, &state->baseline, 0)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper rejected by receiver socket=%s round=%016llx helper=%s source=%s reason=%s",
                   socket->name, (unsigned long long)state->round, sc_public_key_hex(state->current_pair.helper, (char[65]){0}),
                   sockaddr_storage_to_str(source).str, route_ping_addr_equal(source, &state->baseline, 0) ? "hairpin" : "same_observer_ip");
        owner_finish(state, NAT_REASON_NO_HELPER); return;
    }
    if (step == NAT_STEP_PRE || step == NAT_STEP_PREPARE) {
        if (!state->pre_received) {
            if (contacted(state, source)) {
                state->helper_address = *source; state->filter_clean = 0;
                DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT helper rejected by receiver socket=%s round=%016llx source=%s reason=previous_udp_contact",
                           socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(source).str);
                owner_finish(state, NAT_REASON_CONTACT_HISTORY); return;
            }
            state->filter_clean = !contacted(state, source); state->helper_address = *source;
        }
        state->pre_received = 1;
    }
    else {
        uint8_t wire[ROUTE_ADDR_WIRE_SIZE];
        if (!state->history_complete || route_ping_addr_encode(wire, source)) state->filter_clean = 0;
        else for (unsigned i = 0; i < state->contact_cutoff; i++)
            if (!memcmp(state->contacts[i], wire, 17)) state->filter_clean = 0;
        if (!state->filter_clean) DEBUG_DEBUG(DEBUG_CATEGORY_NAT,
            "NAT post-contact ingress cannot establish filtering socket=%s round=%016llx source=%s reason=contact_history",
            socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(source).str);
        state->post_received = 1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT direct ingress socket=%s round=%016llx step=%u source=%s clean=%u",
                socket->name, (unsigned long long)state->round, step, sockaddr_storage_to_str(source).str, state->filter_clean);
}

/* Дополнительный порт наблюдателя не публикуется и не участвует в NCD. */
static struct ETCP_SOCKET *helper_socket(struct NAT_DETECTION *detector, struct ETCP_SOCKET *source) {
    struct nat_socket *state = socket_state(detector, source, 1);
    if (!state) return NULL;
    struct ETCP_SOCKET **slot = &state->helper_primary;
    if (*slot) return *slot;
    struct CFG_SERVER server; memset(&server, 0, sizeof(server));
    server.ip = source->local_addr; server.netif_index = source->netif_index;
    server.type = CFG_SERVER_TYPE_PRIVATE; server.mtu = source->mtu; server.fib = -1;
    for (const struct CFG_SERVER *cfg = detector->instance->config->servers; cfg; cfg = cfg->next)
        if (!strcmp(cfg->name, source->name)) { server.so_mark = cfg->so_mark; server.fib = cfg->fib; break; }
    snprintf(server.name, sizeof(server.name), "nat-helper-%u", source->sock_id);
    if (server.ip.ss_family == AF_INET) ((struct sockaddr_in *)&server.ip)->sin_port = 0;
    else ((struct sockaddr_in6 *)&server.ip)->sin6_port = 0;
    *slot = etcp_socket_add(detector->instance, &server);
    if (!*slot) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT helper socket failed source=%s", source->name); return NULL; }
    (*slot)->probe_only = 1;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT isolated helper socket source=%s local=%s helper=%s",
               source->name, sockaddr_storage_to_str(&source->local_addr).str,
               sockaddr_storage_to_str(&(*slot)->local_addr).str);
    return *slot;
}

static void helper_remove(struct nat_probe *probe) {
    struct NAT_DETECTION *detector = probe->detector;
    struct nat_probe **cursor = &detector->probes;
    while (*cursor && *cursor != probe) cursor = &(*cursor)->next;
    if (*cursor) *cursor = probe->next;
    probe->cancelled = 1;
    etcp_cancel_pings(detector->instance, helper_ping_cb, probe);
    u_free(probe);
}

static void helper_finish(struct nat_probe *probe, uint8_t reason) {
    if (!participants_allowed(probe->detector, probe->group_id, probe->message.owner, probe->message.observer)) {
        helper_remove(probe); return;
    }
    probe->message.command = NAT_RESULT; probe->message.reason = reason;
    if (reason) DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper failed group=%016llx owner=%s round=%016llx step=%u reason=%s",
                          (unsigned long long)probe->group_id, sc_public_key_hex(probe->message.owner, (char[65]){0}),
                          (unsigned long long)read64(probe->message.round), probe->message.step, nat_check_reason_name(reason));
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT helper done owner=%s round=%016llx step=%u sent=%u received=%u errors=%u reason=%s",
                sc_public_key_hex(probe->message.owner, (char[65]){0}), (unsigned long long)read64(probe->message.round),
                probe->message.step, probe->message.sent, probe->message.received, probe->message.errors, nat_check_reason_name(reason));
    if (send_control(probe->detector, probe->group_id, probe->reply, &probe->message))
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper result delivery failed");
    helper_remove(probe);
}

/* Входящая проба несёт идентификаторы owner; OBSERVE с помощника — идентификаторы его собственного socket. */
static void ingress_tag(const struct nat_wire *message, const uint8_t group_key[32], uint8_t tag[NAT_QUERY_TAG_SIZE]) {
    memcpy(tag, "NAT4UDP!", 8); tag[8] = message->owner_socket;
    memcpy(tag + 9, message->generation, 8); memcpy(tag + 17, message->round, 8); memcpy(tag + ROUTE_OBSERVE_GROUP_OFF, group_key, 32);
    tag[ROUTE_OBSERVE_TAG_SIZE] = message->step;
    memcpy(tag + ROUTE_OBSERVE_TAG_SIZE + 1, message->token, 8);
}

static void helper_ping_cb(int success, uint16_t rtt, void *arg, uint64_t nonce, const uint8_t *data, size_t length) {
    (void)rtt; (void)nonce;
    struct nat_probe *probe = arg;
    if (probe->cancelled) return;
    probe->active = 0;
    if (!participants_allowed(probe->detector, probe->group_id, probe->message.owner, probe->message.observer)) {
        helper_remove(probe); return;
    }
    uint8_t step = probe->message.step;
    if (step == NAT_STEP_PREPARE && probe->finding_primary) {
        struct ETCP_SOCKET *socket = find_socket(probe->detector, probe->io_socket, probe->io_generation);
        struct sockaddr_storage mapped;
        if (success && socket && !route_ping_observation(data, length, socket, probe->group_id,
                                                        read64(probe->message.round), &mapped) && public_address(probe->detector, &mapped)) {
            if (route_ping_addr_encode(probe->message.primary, &mapped)) { helper_finish(probe, NAT_REASON_INVALID_OBSERVATION); return; }
            probe->message.received++;
            DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT helper endpoint measured group=%016llx owner=%s step=%u socket=%s mapped=%s fallback=%u",
                        (unsigned long long)probe->group_id, sc_public_key_hex(probe->message.owner, (char[65]){0}), step,
                        socket->name, sockaddr_storage_to_str(&mapped).str, probe->finding_primary);
            helper_finish(probe, NAT_REASON_NONE); return;
        }
        if (success) { helper_finish(probe, NAT_REASON_INVALID_OBSERVATION); return; }
        if (probe->message.sent >= NAT_PROBE_COUNT) { helper_finish(probe, NAT_REASON_TIMEOUT); return; }
    } else {
        if (success) DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT one-way ingress unexpectedly received PONG group=%016llx step=%u",
                               (unsigned long long)probe->group_id, step);
        if (step == NAT_STEP_PREPARE && probe->message.sent >= NAT_PROBE_COUNT) {
            probe->finding_primary = 1; probe->message.sent = 0;
            DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT initial ingress series sent; discover helper endpoint through broker group=%016llx owner=%s",
                        (unsigned long long)probe->group_id, sc_public_key_hex(probe->message.owner, (char[65]){0}));
        }
        if (probe->message.sent >= NAT_PROBE_COUNT) { helper_finish(probe, NAT_REASON_NONE); return; }
    }
    probe->due = get_time_tb() + 1000;
}

static int helper_send(struct nat_probe *probe) {
    struct NAT_DETECTION *detector = probe->detector;
    if (!participants_allowed(detector, probe->group_id, probe->message.owner, probe->message.observer)) return -1;
    struct sockaddr_storage target;
    if (route_ping_addr_decode(probe->message.target, &target)) return -1;
    int timeout = ((unsigned)probe->message.timeout_ms[0] << 8) | probe->message.timeout_ms[1];
    if (timeout < 500 || timeout > NAT_ROUND_TB / 10) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper has invalid timeout_ms=%d", timeout); return -1;
    }
    int result;
    if (probe->message.step == NAT_STEP_PREPARE && probe->finding_primary) {
        struct ETCP_SOCKET *source = find_socket(detector, probe->source_socket, probe->source_generation);
        if (!source) { DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper source generation disappeared"); return -1; }
        struct ETCP_SOCKET *socket = helper_socket(detector, source);
        if (!socket) return -1;
        if (route_ping_addr_decode(probe->message.observer_address, &target)) return -1;
        probe->io_socket = socket->sock_id; probe->io_generation = socket->generation;
        result = observe_control(socket, probe->observer_key, &target, probe->group_id, read64(probe->message.round),
                                  NAT_STEP_HELPER_OBSERVE, read64(probe->message.token), timeout, helper_ping_cb, probe);
    } else {
        struct ETCP_SOCKET *source = find_socket(detector, probe->source_socket, probe->source_generation);
        if (!source || source->is_tcp || source->socks_udp) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper source is unavailable socket=%u", probe->source_socket); return -1;
        }
        struct ETCP_SOCKET *socket = source;
        if (probe->observer_generation) {
            struct ETCP_SOCKET *observer = find_socket(detector, probe->observer_socket, probe->observer_generation);
            if (!direct_udp_socket(detector, observer) || !direct_udp_socket(detector, source)) {
                DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT public UDP pair no longer usable observer=%u helper=%u round=%016llx",
                           probe->observer_socket, probe->source_socket, (unsigned long long)read64(probe->message.round));
                return -1;
            }
        } else socket = helper_socket(detector, source);
        if (!socket) return -1;
        probe->io_socket = socket->sock_id; probe->io_generation = socket->generation;
        uint8_t tag[NAT_QUERY_TAG_SIZE];
        struct TOPO_GROUP *group = topo_groups_find(probe->detector->instance->topo_groups, probe->group_id);
        const uint8_t *key = topo_group_wire_id(group);
        if (!key || group->stopping) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper ingress group unavailable route=%llu", (unsigned long long)probe->group_id); return -1;
        }
        ingress_tag(&probe->message, key, tag);
        result = etcp_send_ping_to_socket(detector->instance, socket, probe->owner_key, &target, timeout,
                                          helper_ping_cb, probe, tag, sizeof(tag), ETCP_PING_FLAG_ADDRESS_CHECK | ETCP_PING_FLAG_NO_REPLY);
    }
    if (result) { probe->message.errors++; return -1; }
    probe->message.sent++; probe->active = 1; return 0;
}

/* Broker проверяет реальное наблюдение owner, затем делегирует выбранному владельцем помощнику через ту же группу. */
static void handle_run(struct NAT_DETECTION *detector, uint64_t group_id, const uint8_t sender[32], struct nat_wire *message) {
    const uint8_t *owner = message->owner, *observer = message->observer, *helper = message->helper;
    const uint8_t *self = detector->instance->my_ed25519_pubkey;
    const struct TOPO_NODE *owner_node = group_node(detector, group_id, owner);
    const struct TOPO_NODE *observer_node = group_node(detector, group_id, observer);
    const struct TOPO_NODE *helper_node = group_node(detector, group_id, helper);
    struct sockaddr_storage target;
    uint8_t reason = NAT_REASON_INVALID_OBSERVATION;
    int shared = !memcmp(observer, helper, 32);
    struct nat_binding *binding = NULL;
    if (!owner_node || !observer_node || !helper_node || !memcmp(owner, observer, 32) || !memcmp(owner, helper, 32) ||
        route_ping_addr_decode(message->target, &target) || !public_address(detector, &target)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT run has invalid participants/target group=%016llx owner=%s observer=%s helper=%s",
                   (unsigned long long)group_id, sc_public_key_hex(owner, (char[65]){0}), sc_public_key_hex(observer, (char[65]){0}),
                   sc_public_key_hex(helper, (char[65]){0})); goto reject;
    }
    if (!memcmp(sender, owner, 32) && !memcmp(observer, self, 32)) {
        binding = binding_find(detector, group_id, owner, read64(message->generation), read64(message->round));
        if (!binding || binding->owner_socket != message->owner_socket || !route_ping_addr_equal(&binding->target, &target, 1)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT delegation rejected: stale own observation group=%016llx owner=%s",
                       (unsigned long long)group_id, sc_public_key_hex(owner, (char[65]){0})); goto reject;
        }
        if (!shared) {
            memcpy(binding->helper, helper, 32); binding->token = read64(message->token); binding->expires = get_time_tb() + NAT_ROUND_TB;
            if (!control_route(detector, group_id, helper) || send_control(detector, group_id, helper, message)) {
                DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT delegation route failed group=%016llx helper=%s",
                           (unsigned long long)group_id, sc_public_key_hex(helper, (char[65]){0}));
                reason = NAT_REASON_CONTROL_LOST; goto reject;
            }
            return;
        }
    } else if (shared || memcmp(sender, observer, 32) || memcmp(helper, self, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT run role mismatch group=%016llx sender=%s observer=%s helper=%s",
                   (unsigned long long)group_id, sc_public_key_hex(sender, (char[65]){0}), sc_public_key_hex(observer, (char[65]){0}),
                   sc_public_key_hex(helper, (char[65]){0})); goto reject;
    }
    unsigned count = 0;
    for (struct nat_probe *probe = detector->probes; probe; probe = probe->next) {
        count++;
        if (probe->group_id == group_id && !memcmp(probe->message.owner, owner, 32) &&
            read64(probe->message.generation) == read64(message->generation) && read64(probe->message.round) == read64(message->round) &&
            read64(probe->message.token) == read64(message->token)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT duplicate helper run ignored"); return;
        }
    }
    if (count >= NAT_MAX_PROBES) { DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper probe limit reached"); reason = NAT_REASON_SEND_ERROR; goto reject; }
    struct sockaddr_storage observer_address;
    struct ETCP_SOCKET *source = find_socket(detector, message->helper_socket, 0);
    if (!source || source->is_tcp || source->probe_only || source->socks_udp ||
        source->type == CFG_SERVER_TYPE_PRIVATE || source->type == CFG_SERVER_TYPE_LOCAL ||
        source->local_addr.ss_family != target.ss_family ||
        route_ping_addr_decode(message->observer_address, &observer_address) ||
        observer_address.ss_family != target.ss_family || !public_address(detector, &observer_address)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT helper source is not a native socket group=%016llx socket=%u",
                   (unsigned long long)group_id, message->helper_socket);
        reason = source && source->socks_udp ? NAT_REASON_PROXY : NAT_REASON_NO_HELPER; goto reject;
    }
    struct ETCP_SOCKET *observer_socket = NULL;
    if (shared) {
        struct sockaddr_storage alternate;
        observer_socket = find_socket(detector, binding->observer_socket, binding->observer_generation);
        if (message->step == NAT_STEP_PREPARE || !direct_udp_socket(detector, observer_socket) ||
            !direct_udp_socket(detector, source) || route_ping_addr_decode(message->primary, &alternate) ||
            !route_ping_addr_equal(&observer_address, &observer_socket->local_addr, 1) ||
            !route_ping_addr_equal(&alternate, &source->local_addr, 1) ||
            route_ping_addr_equal(&observer_address, &alternate, 0) || route_ping_addr_equal(&target, &alternate, 0)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT public UDP pair rejected group=%016llx round=%016llx observer=%s helper_socket=%u",
                       (unsigned long long)group_id, (unsigned long long)read64(message->round),
                       sockaddr_storage_to_str(&observer_address).str, message->helper_socket);
            reason = NAT_REASON_NO_HELPER; goto reject;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT public UDP pair serving group=%016llx round=%016llx step=%u observer=%s helper=%s target=%s",
                    (unsigned long long)group_id, (unsigned long long)read64(message->round), message->step,
                    sockaddr_storage_to_str(&observer_address).str, sockaddr_storage_to_str(&alternate).str,
                    sockaddr_storage_to_str(&target).str);
    }
    struct nat_probe *probe = u_calloc(1, sizeof(*probe));
    if (!probe) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT helper probe allocation failed"); reason = NAT_REASON_SEND_ERROR; goto reject; }
    probe->detector = detector; probe->message = *message; probe->group_id = group_id; memcpy(probe->reply, sender, 32);
    memcpy(probe->owner_key, owner_node->public_key, 32); memcpy(probe->observer_key, observer_node->public_key, 32);
    probe->source_socket = source->sock_id; probe->source_generation = source->generation;
    if (observer_socket) {
        probe->observer_socket = observer_socket->sock_id; probe->observer_generation = observer_socket->generation;
    }
    probe->expires = get_time_tb() + NAT_ROUND_TB;
    probe->next = detector->probes; detector->probes = probe;
    if (helper_send(probe)) helper_finish(probe, NAT_REASON_SEND_ERROR);
    return;
reject:
    message->command = NAT_RESULT; message->reason = reason;
    if (send_control(detector, group_id, sender, message)) DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT rejection delivery failed");
}

static void handle_result(struct nat_socket *state, const struct nat_wire *message) {
    if (!participants_allowed(state->detector, state->group_id, state->current_pair.observer, state->current_pair.helper)) {
        owner_finish(state, NAT_REASON_CANCELLED); return;
    }
    uint8_t step = state->phase == NAT_PRE ? NAT_STEP_PRE : state->phase == NAT_POST ? NAT_STEP_POST :
                   state->phase == NAT_PREPARE_C ? NAT_STEP_PREPARE : 0;
    if (!step || message->step != step || read64(message->token) != state->token || memcmp(message->helper, state->current_pair.helper, 32)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT stale helper result ignored socket=%s phase=%u", state->socket->name, state->phase); return;
    }
    if (get_time_tb() >= state->cycle_deadline) { owner_finish(state, NAT_REASON_BUDGET); return; }
    if (message->reason || message->errors || !message->sent || message->received > message->sent) {
        owner_finish(state, message->reason ? message->reason : NAT_REASON_SEND_ERROR); return;
    }
    if (single_peer_pair(&state->current_pair)) {
        struct sockaddr_storage alternate;
        if (message->helper_socket != state->helper_socket || route_ping_addr_decode(message->primary, &alternate) ||
            !route_ping_addr_equal(&alternate, &state->helper_address, 1)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT public UDP pair result has wrong helper socket=%s round=%016llx expected=%s helper_socket=%u",
                       state->socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(&state->helper_address).str,
                       message->helper_socket);
            owner_finish(state, NAT_REASON_INVALID_OBSERVATION); return;
        }
    }
    if (step == NAT_STEP_PREPARE) {
        struct sockaddr_storage primary;
        if (route_ping_addr_decode(message->primary, &primary) || primary.ss_family != state->local.ss_family ||
            !public_address(state->detector, &primary)) { owner_finish(state, NAT_REASON_INVALID_OBSERVATION); return; }
        if (state->pre_received) primary = state->helper_address; /* Source полученной пробы важнее объявленного mapping помощника. */
        int same_ip = route_ping_addr_equal(&state->observer_address, &primary, 0) ||
                      route_ping_addr_equal(&state->baseline, &primary, 0);
        int old_contact = state->pre_received ? !state->filter_clean : contacted(state, &primary);
        if (same_ip) {
            DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT try another pair socket=%s group=%016llx helper=%s endpoint=%s reason=%s",
                       state->socket->name, (unsigned long long)state->group_id, sc_public_key_hex(state->current_pair.helper, (char[65]){0}),
                       sockaddr_storage_to_str(&primary).str, "same_external_ip");
            owner_finish(state, NAT_REASON_NO_HELPER); return;
        }
        state->helper_address = primary; state->filter_clean = !old_contact;
        if (old_contact) DEBUG_INFO(DEBUG_CATEGORY_NAT,
            "NAT known helper used for mapping only socket=%s round=%016llx helper=%s",
            state->socket->name, (unsigned long long)state->round, sockaddr_storage_to_str(&primary).str);
        if (state->pre_received || old_contact) {
            state->phase = NAT_OBSERVE_C;
            if (owner_observe(state)) owner_finish(state, NAT_REASON_SEND_ERROR);
            return;
        }
        if (owner_run(state, NAT_STEP_PRE)) owner_finish(state, NAT_REASON_CONTROL_LOST);
        return;
    }
    state->pre_healthy = step == NAT_STEP_PRE ? message->sent == NAT_PROBE_COUNT &&
        state->heartbeat_received >= state->pre_started &&
        get_time_tb() - state->heartbeat_received <= (uint64_t)probe_timeout_ms(state) * 20 : state->pre_healthy;
    state->phase = step == NAT_STEP_PRE ? NAT_PRE_WAIT : NAT_POST_WAIT;
    state->due = get_time_tb() + (uint64_t)probe_timeout_ms(state) * 10; /* Router может обогнать последнюю UDP-пробу. */
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT ingress series complete socket=%s round=%016llx step=%u sent=%u pre=%u post=%u clean=%u anchor=%u",
                state->socket->name, (unsigned long long)state->round, step, message->sent,
                state->pre_received, state->post_received, state->filter_clean, state->pre_healthy);
}

/* Отчёт привязан к точному UDP-запросу и подписан наблюдателем; старый этап не завершит новый запрос. */
static void receive_observation(struct NAT_DETECTION *detector, uint64_t group, const uint8_t sender[32], const struct nat_wire *message) {
    if (memcmp(message->owner, detector->instance->my_ed25519_pubkey, 32) || memcmp(message->observer, sender, 32) ||
        memcmp(message->helper, sender, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT observation report has invalid participants group=%016llx sender=%s",
                   (unsigned long long)group, sc_public_key_hex(sender, (char[65]){0})); return;
    }
    struct ETCP_SOCKET *socket = find_socket(detector, message->owner_socket, read64(message->generation));
    if (!socket) { DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT observation report for old socket ignored"); return; }
    uint8_t data[ROUTE_ADDR_WIRE_SIZE + ROUTE_OBSERVE_TAG_SIZE];
    memcpy(data, message->target, ROUTE_ADDR_WIRE_SIZE);
    route_ping_make_tag(data + ROUTE_ADDR_WIRE_SIZE, socket, group, read64(message->round));
    if (message->step == NAT_STEP_HELPER_OBSERVE) {
        for (struct nat_probe *probe = detector->probes; probe; probe = probe->next) {
            if (!probe->active || !probe->finding_primary || probe->group_id != group || probe->io_socket != socket->sock_id ||
                probe->io_generation != socket->generation || read64(probe->message.round) != read64(message->round) ||
                read64(probe->message.token) != read64(message->token) || memcmp(probe->message.observer, sender, 32)) continue;
            etcp_cancel_pings(detector->instance, helper_ping_cb, probe);
            if (message->reason) { helper_finish(probe, message->reason); return; }
            helper_ping_cb(1, 0, probe, 0, data, sizeof(data)); return;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT stale helper observation report ignored"); return;
    }
    struct nat_socket *state = socket_state(detector, socket, 0);
    if (!state || !state->active_ping || state->group_id != group || state->round != read64(message->round) ||
        state->observation_token != read64(message->token) || state->phase != message->step ||
        memcmp((state->phase == NAT_OBSERVE_C || state->phase == NAT_REPEAT_C) ?
            state->current_pair.helper : state->current_pair.observer, sender, 32)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT stale/foreign observation report ignored group=%016llx sender=%s step=%u",
                    (unsigned long long)group, sc_public_key_hex(sender, (char[65]){0}), message->step); return;
    }
    if (get_time_tb() >= state->cycle_deadline) { owner_finish(state, NAT_REASON_BUDGET); return; }
    if (single_peer_pair(&state->current_pair)) {
        const struct sockaddr_storage *expected = state->phase == NAT_OBSERVE_C || state->phase == NAT_REPEAT_C ?
            &state->helper_address : &state->observer_address;
        struct sockaddr_storage receiver;
        if (route_ping_addr_decode(message->observer_address, &receiver) || !route_ping_addr_equal(expected, &receiver, 1)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT report has wrong UDP receiver socket=%s round=%016llx phase=%u receiver=%s expected=%s",
                       state->socket->name, (unsigned long long)state->round, state->phase,
                       sockaddr_storage_to_str(&receiver).str, sockaddr_storage_to_str(expected).str);
            owner_finish(state, NAT_REASON_INVALID_OBSERVATION); return;
        }
    }
    etcp_cancel_pings(detector->instance, owner_ping_cb, state);
    if (message->reason) { owner_finish(state, message->reason); return; }
    owner_ping_cb(1, 0, state, 0, data, sizeof(data));
}

/* Потеря router-сессии отменяет только роли этого узла в данной группе, не общий транспорт. */
static void cancel_route(struct NAT_DETECTION *detector, uint64_t group_id, const uint8_t peer[32]) {
    for (struct nat_socket *state = detector->sockets; state; state = state->next)
        if (state->phase && state->group_id == group_id && (!memcmp(state->current_pair.observer, peer, 32) || !memcmp(state->current_pair.helper, peer, 32)))
            owner_finish(state, participants_allowed(detector, group_id, state->current_pair.observer, state->current_pair.helper) ?
                         NAT_REASON_CONTROL_LOST : NAT_REASON_CANCELLED);
    struct nat_probe *probe = detector->probes;
    while (probe) {
        struct nat_probe *next = probe->next;
        if (probe->group_id == group_id && (!memcmp(probe->reply, peer, 32) || !memcmp(probe->message.owner, peer, 32))) helper_remove(probe);
        probe = next;
    }
    for (struct nat_binding *binding = detector->bindings; binding; binding = binding->next)
        if (binding->group_id == group_id && (!memcmp(binding->owner, peer, 32) || !memcmp(binding->helper, peer, 32))) binding->expires = 0;
}

/* SOURCE и group берём из проверенного router-заголовка, а не из ETCP next hop. */
static void receive_control(struct ETCP_CONN *from, struct ll_entry *entry) {
    struct NAT_DETECTION *detector = from && from->instance ? from->instance->nat_det : NULL;
    if (!detector || !entry || !entry->dgram || entry->len < ROUTER_SVC_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT routed control invalid size=%u", entry ? (unsigned)entry->len : 0u);
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); } return;
    }
    uint64_t group_id; uint8_t sender[32];
    memcpy(&group_id, entry->dgram + ROUTER_SVC_GROUP_OFF, 8); memcpy(sender, entry->dgram + ROUTER_SVC_SRC_ED_OFF, 32);
    if (entry->len == ROUTER_SVC_HDR_SIZE) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT routed control closed group=%016llx peer=%s",
                    (unsigned long long)group_id, sc_public_key_hex(sender, (char[65]){0}));
        queue_dgram_free(entry); queue_entry_free(entry); cancel_route(detector, group_id, sender); return;
    }
    if (entry->dgram[ROUTER_SVC_FLAGS_OFF] != (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        !group_node(detector, group_id, sender)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT routed control rejected group=%016llx sender=%s size=%u flags=%u",
                   (unsigned long long)group_id, sc_public_key_hex(sender, (char[65]){0}), (unsigned)entry->len, entry->dgram[ROUTER_SVC_FLAGS_OFF]);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    if (entry->len == ROUTER_SVC_HDR_SIZE + sizeof(struct nat_recheck_wire)) {
        struct nat_recheck_wire request; memcpy(&request, entry->dgram + ROUTER_SVC_PAYLOAD_OFF, sizeof(request));
        queue_dgram_free(entry); queue_entry_free(entry);
        struct ETCP_SOCKET *socket = find_socket(detector, request.socket_id, 0);
        struct sockaddr_storage address = {0};
        if (request.cmd != ETCP_RT_ID_NAT_DETECTION || request.command != NAT_RECHECK || request.version != NAT_WIRE_VERSION ||
            !socket || socket->is_tcp || socket->probe_only || socket->socks_udp ||
            socket->type == CFG_SERVER_TYPE_LOCAL || socket->type == CFG_SERVER_TYPE_PRIVATE ||
            route_ping_addr_decode(request.address, &address) ||
            (!route_ping_addr_equal(&address, &socket->local_addr, 1) && !route_ping_addr_equal(&address, &socket->nat_addr, 1))) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT failure recheck rejected group=%016llx peer=%s socket=%u: stale/invalid endpoint",
                       (unsigned long long)group_id, sc_public_key_hex(sender, (char[65]){0}), request.socket_id); return;
        }
        schedule_failure_recheck(socket_state(detector, socket, 1), sender, 1); return;
    }
    if (entry->len != ROUTER_SVC_HDR_SIZE + sizeof(struct nat_wire)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT control invalid payload size=%u", (unsigned)(entry->len - ROUTER_SVC_HDR_SIZE));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    struct nat_wire message; memcpy(&message, entry->dgram + ROUTER_SVC_PAYLOAD_OFF, sizeof(message));
    queue_dgram_free(entry); queue_entry_free(entry);
    if (message.cmd != ETCP_RT_ID_NAT_DETECTION || message.version != NAT_WIRE_VERSION ||
        key_empty(message.owner) || key_empty(message.observer) || key_empty(message.helper) ||
        !read64(message.generation) || !read64(message.round) || !read64(message.token) ||
        (message.command != NAT_RUN && message.command != NAT_RESULT && message.command != NAT_OBSERVATION) ||
        message.reason > NAT_REASON_DEPENDENT_MAPPING ||
        (message.command != NAT_OBSERVATION && (message.step < NAT_STEP_PREPARE || message.step > NAT_STEP_POST))) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT control malformed command=%u version=%u step=%u", message.command, message.version, message.step);
        return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT control RX group=%016llx command=%u step=%u sender=%s via=%s owner=%s round=%016llx",
                (unsigned long long)group_id, message.command, message.step, sc_public_key_hex(sender, (char[65]){0}),
                sc_public_key_hex(from->peer_ed25519_pubkey, (char[65]){0}), sc_public_key_hex(message.owner, (char[65]){0}), (unsigned long long)read64(message.round));
    if (message.command == NAT_RUN) { handle_run(detector, group_id, sender, &message); return; }
    if (message.command == NAT_OBSERVATION) { receive_observation(detector, group_id, sender, &message); return; }
    const uint8_t *owner = message.owner;
    if (memcmp(owner, detector->instance->my_ed25519_pubkey, 32)) {
        struct nat_binding *binding = binding_find(detector, group_id, owner, read64(message.generation), read64(message.round));
        if (!binding || memcmp(binding->helper, sender, 32) || memcmp(message.helper, sender, 32) || binding->token != read64(message.token) ||
            memcmp(message.observer, detector->instance->my_ed25519_pubkey, 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT result has no matching delegation group=%016llx owner=%s sender=%s",
                       (unsigned long long)group_id, sc_public_key_hex(owner, (char[65]){0}), sc_public_key_hex(sender, (char[65]){0})); return;
        }
        if (send_control(detector, group_id, owner, &message)) DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT result forwarding failed");
        return;
    }
    struct ETCP_SOCKET *socket = find_socket(detector, message.owner_socket, read64(message.generation));
    struct nat_socket *state = socket_state(detector, socket, 0);
    if (!state || state->group_id != group_id || state->round != read64(message.round) ||
        memcmp(state->current_pair.observer, sender, 32) || memcmp(message.observer, sender, 32) || state->phase == NAT_IDLE) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT stale/foreign owner response ignored group=%016llx", (unsigned long long)group_id); return;
    }
    handle_result(state, &message);
}

static void owner_start(struct nat_socket *state) {
    struct NAT_DETECTION *detector = state->detector;
    if (state->socket->is_tcp) {
        select_profile(state); state->due = UINT64_MAX;
        state->reason = state->socket->address_profile.source ? NAT_REASON_NONE : NAT_REASON_NO_OBSERVER;
        return;
    }
    if (state->socket->socks_udp) {
        state->reason = NAT_REASON_PROXY; state->due = UINT64_MAX;
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT native endpoint not measured through SOCKS5 socket=%s", state->socket->name); return;
    }
    if (state->socket->type == CFG_SERVER_TYPE_LOCAL || state->socket->type == CFG_SERVER_TYPE_PRIVATE) {
        state->due = UINT64_MAX; return;
    }
    uint64_t now = get_time_tb();
    history_load(state);
    int full_budget = !state->cycling || now >= state->cycle_deadline;
    if (!state->cycling) {
        state->cycling = 1; state->cursor = (struct nat_pair){0};
        state->cycle_contact_cutoff = state->contact_count; state->cycle_history_complete = state->history_complete;
        state->cycle_mapping_seen = 0; state->reason = NAT_REASON_NONE;
        state->cycle_deadline = now + NAT_ROUND_TB;
    } else if (now >= state->cycle_deadline) {
        state->cycle_deadline = now + NAT_ROUND_TB;
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT candidate cycle resumed socket=%s contacts_at_start=%u current_contacts=%u",
                   state->socket->name, state->cycle_contact_cutoff, state->contact_count);
    }
    if (select_pair(state)) { owner_finish(state, state->reason); return; }
    state->pair_full_budget = full_budget;
    if (random_id(&state->round)) { owner_finish(state, NAT_REASON_SEND_ERROR); return; }
    state->rounds++;
    memset(&state->helper_seen, 0, sizeof(state->helper_seen)); memset(&state->baseline, 0, sizeof(state->baseline));
    state->retries = 0; state->pre_received = 0; state->post_received = 0; state->filter_clean = 1; state->pre_healthy = 0;
    state->heartbeat_due = get_time_tb(); state->heartbeat_received = 0; state->pre_started = 0;
    state->deadline = state->cycle_deadline; state->reason = NAT_REASON_NONE;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT round start socket=%s group=%016llx id=%u generation=%016llx round=%016llx observer=%s helper=%s protocol=%s",
               state->socket->name, (unsigned long long)state->group_id, state->socket->sock_id,
               (unsigned long long)state->socket->generation, (unsigned long long)state->round,
               sc_public_key_hex(state->current_pair.observer, (char[65]){0}), sc_public_key_hex(state->current_pair.helper, (char[65]){0}), state->socket->is_tcp ? "TCP" : "UDP");
    state->phase = NAT_OBSERVE_B;
    if (owner_observe(state)) owner_finish(state, NAT_REASON_SEND_ERROR);
}

static void detector_tick(void *arg) {
    struct NAT_DETECTION *detector = arg; detector->timer = NULL;
    if (detector->publish_pending && get_time_tb() >= detector->publish_after) publish(detector);
    uint64_t now = get_time_tb();
    for (struct ETCP_SOCKET *socket = detector->instance->etcp_sockets; socket; socket = socket->next)
        if (!socket->probe_only) socket_state(detector, socket, 1);
    for (struct nat_socket *state = detector->sockets; state; state = state->next) {
        if (now >= state->select_due) { select_profile(state); state->select_due = now + 10000; }
        if (state->phase && !participants_allowed(detector, state->group_id, state->current_pair.observer, state->current_pair.helper)) {
            owner_finish(state, NAT_REASON_CANCELLED); continue;
        }
        if (state->phase && now >= state->deadline) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT phase deadline socket=%s round=%016llx phase=%u cycle_remaining_tb=%llu",
                       state->socket->name, (unsigned long long)state->round, state->phase,
                       (unsigned long long)(state->cycle_deadline > now ? state->cycle_deadline - now : 0));
            owner_finish(state, now >= state->cycle_deadline ? NAT_REASON_BUDGET : NAT_REASON_TIMEOUT); continue;
        }
        if (!state->phase && now >= state->due && (detector->automatic || state->forced)) {
            state->forced = 0; owner_start(state);
        }
        else if (state->phase && !state->socket->is_tcp) {
            if ((state->phase == NAT_PRE_WAIT || state->phase == NAT_POST_WAIT) && now >= state->due) {
                state->phase = state->phase == NAT_PRE_WAIT ? NAT_OBSERVE_C : NAT_REPEAT_B; state->retries = 0;
                if (owner_observe(state)) { owner_finish(state, NAT_REASON_SEND_ERROR); continue; }
            }
            if (!state->heartbeat_active && state->baseline.ss_family && now >= state->heartbeat_due) {
                if (route_ping_observe(state->socket, state->observer_key, &state->observer_address, state->group_id, state->round,
                                       probe_timeout_ms(state), owner_heartbeat_cb, state)) {
                    DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT anchor probe send failed socket=%s round=%016llx",
                               state->socket->name, (unsigned long long)state->round);
                    state->heartbeat_due = now + 10000;
                } else state->heartbeat_active = 1;
            }
        }
        if ((state->phase == NAT_OBSERVE_B || state->phase == NAT_OBSERVE_C || state->phase == NAT_REPEAT_C ||
             state->phase == NAT_REPEAT_B) && !state->active_ping && now >= state->due) {
            if (owner_observe(state)) owner_finish(state, NAT_REASON_SEND_ERROR);
        }
    }
    struct nat_probe *probe = detector->probes;
    while (probe) {
        struct nat_probe *next = probe->next;
        if (now >= probe->expires) helper_finish(probe, NAT_REASON_TIMEOUT);
        else if (!probe->active && now >= probe->due && helper_send(probe)) helper_finish(probe, NAT_REASON_SEND_ERROR);
        probe = next;
    }
    struct nat_binding **cursor = &detector->bindings;
    while (*cursor) {
        struct nat_binding *binding = *cursor;
        if (now < binding->expires) { cursor = &binding->next; continue; }
        *cursor = binding->next; u_free(binding);
    }
    detector->timer = uasync_set_timeout(detector->instance->ua, NAT_TICK_TB, detector, detector_tick, "nat_detection");
    if (!detector->timer) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT detector timer scheduling failed");
}

/* Отпечаток доступных endpoint не содержит timestamp/имя/профиль: обычный повтор NODEINFO не запускает пробы. */
static uint64_t candidate_fingerprint(struct nat_socket *state, int ready) {
    struct NAT_DETECTION *detector = state->detector;
    uint64_t total = 0;
    for (struct ll_entry *e = detector->instance->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP *group = (struct TOPO_GROUP *)e;
        if (group->stopping || !group->local_node) continue;
        uint64_t group_total = 0; unsigned candidates = 0, observers = 0, peers_with_pairs = 0;
        for (struct ll_entry *n = group->nodes->head; n; n = n->next) {
            const uint8_t *ed = ((struct TOPO_GROUP_NODE *)n)->ed25519;
            if (!memcmp(ed, detector->instance->my_ed25519_pubkey, 32) ||
                (ready ? !control_route(detector, group->group_id, ed) : !topo_node_best_path((struct TOPO_GROUP_NODE *)n))) continue;
            const struct TOPO_NODE *node = group_node(detector, group->group_id, ed);
            if (!node) continue;
            struct sockaddr_storage endpoint = {0}; uint8_t socket_id = 0;
            uint8_t bytes[40 + ROUTE_ADDR_WIRE_SIZE + 2] = {0};
            write64(bytes, group->group_id); memcpy(bytes + 8, ed, 32);
            struct sockaddr_storage observer_address = {0}; uint8_t observer_socket = 0;
            observers += node_udp_endpoint(detector, node, state->local.ss_family, NAT_ENDPOINT_PUBLIC,
                                           NULL, &observer_address, &observer_socket, NULL);
            candidates++;
            uint8_t previous[ROUTE_ADDR_WIRE_SIZE] = {0}, first_direct[ROUTE_ADDR_WIRE_SIZE] = {0}, first_direct_socket = 0;
            int direct, has_pair = 0;
            int more = node_udp_endpoint(detector, node, state->local.ss_family, NAT_ENDPOINT_ANY, previous, &endpoint, &socket_id, &direct);
            for (; more; more =
                 node_udp_endpoint(detector, node, state->local.ss_family, NAT_ENDPOINT_ANY, previous, &endpoint, &socket_id, &direct)) {
                if (route_ping_addr_encode(previous, &endpoint)) break;
                if (direct && !first_direct[0]) { memcpy(first_direct, previous, sizeof(first_direct)); first_direct_socket = socket_id; }
                if (direct && socket_id != first_direct_socket && memcmp(first_direct, previous, 17)) has_pair = 1;
                memcpy(bytes + 40, previous, sizeof(previous)); bytes[40 + ROUTE_ADDR_WIRE_SIZE] = socket_id;
                bytes[41 + ROUTE_ADDR_WIRE_SIZE] = direct;
                uint64_t hash = UINT64_C(14695981039346656037);
                for (unsigned i = 0; i < sizeof(bytes); i++) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
                group_total += hash;
            }
            peers_with_pairs += has_pair;
        }
        if (ready || (candidates >= 2 && observers) || peers_with_pairs) total += group_total;
        if (!ready) DEBUG_TRACE(DEBUG_CATEGORY_NAT,
                                "NAT cold group candidates node=%s socket=%s group=%016llx candidates=%u observers=%u peers_with_pairs=%u",
                                sc_public_key_hex(detector->instance->my_ed25519_pubkey, (char[65]){0}), state->socket->name,
                                (unsigned long long)group->group_id, candidates, observers, peers_with_pairs);
    }
    return total;
}

/* Ограниченное окно сохраняет свежие IP для первого ingress; изменение доступных endpoint обновляет резерв один раз. */
uint64_t nat_detection_probe_delay(struct NAT_DETECTION *detector) {
    if (!detector || detector->stopping) return 0;
    uint64_t now = get_time_tb(), delay = 0;
    if (detector->reservation_dirty) {
        detector->reservation_dirty = 0;
        for (struct ETCP_SOCKET *socket = detector->instance->etcp_sockets; socket; socket = socket->next) {
            if (socket->is_tcp || socket->probe_only || socket->socks_udp ||
                socket->type == CFG_SERVER_TYPE_PRIVATE || socket->type == CFG_SERVER_TYPE_LOCAL) continue;
            struct nat_socket *state = socket_state(detector, socket, 1);
            if (!state) continue;
            uint64_t fingerprint = candidate_fingerprint(state, 0);
            if (fingerprint != state->reservation_fingerprint) {
                DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT cold reservation node=%s socket=%s previous=%016llx current=%016llx",
                            sc_public_key_hex(detector->instance->my_ed25519_pubkey, (char[65]){0}), socket->name,
                            (unsigned long long)state->reservation_fingerprint, (unsigned long long)fingerprint);
                state->reservation_fingerprint = fingerprint;
                if (fingerprint) state->cold_until = now + NAT_ROUND_TB + 2500;
            }
        }
    }
    for (struct nat_socket *state = detector->sockets; state; state = state->next) {
        if (state->socket->is_tcp || !state->reservation_fingerprint || state->cold_until <= now ||
            (!detector->automatic && !state->phase && !state->forced) ||
            (!state->phase && !state->cycling && !state->forced && state->measured_profile.source == ADDRESS_SOURCE_ETCP &&
             state->measured_profile.mapping && state->measured_profile.inbound))
            continue;
        if (state->cold_until - now > delay) delay = state->cold_until - now;
    }
    return delay > 10000 ? 10000 : delay;
}

/* До READY второй IP ждёт первый линк ограниченное время. Групповая проверка
 * резервирует опубликованную пару, чтобы handshake не открыл фильтр раньше пробы. */
static uint64_t alternate_init_delay(struct NAT_DETECTION *detector, const struct ETCP_CONN *connection,
                                     struct ETCP_SOCKET *socket, const struct sockaddr_storage *remote, uint64_t until) {
    if (!detector || detector->stopping || !socket || socket->is_tcp || socket->probe_only || socket->socks_udp || socket->only_local ||
        socket->type == CFG_SERVER_TYPE_LOCAL || socket->type == CFG_SERVER_TYPE_PRIVATE || !public_address(detector, remote)) return 0;
    struct nat_socket *state = socket_state(detector, socket, 0);
    if (!detector->automatic && (!state || (!state->forced && !state->cycling))) return 0;
    if (state && (state->history_incomplete || state->measured_profile.mapping == ADDRESS_MAP_DEPENDENT ||
        (state->measured_profile.mapping && state->measured_profile.inbound))) return 0;
    uint8_t destination[ROUTE_ADDR_WIRE_SIZE];
    if (route_ping_addr_encode(destination, remote)) return 0;
    for (unsigned i = 0; state && i < state->contact_count; i++)
        if (!memcmp(state->contacts[i], destination, 17)) return 0;

    const uint8_t *key = connection->crypto_ctx.peer_public_key;
    const struct TOPO_NODE *peer = topo_node_registry_lookup(detector->instance->topo_groups, connection->peer_node_id, key);
    int signed_peer = peer && peer->timestamp;
    int target = !signed_peer, anchor = 0;
    for (const struct ETCP_LINK *other = connection->links; other; other = other->next) {
        if (other->conn != socket || other->closing || (until ? (!other->init_retry_count && !other->initialized) : !other->link_status) ||
            route_ping_addr_equal(&other->remote_addr, remote, 0)) continue;
        if (!signed_peer && public_address(detector, &other->remote_addr)) { anchor = 1; break; }
        struct sockaddr_storage candidate;
        uint8_t previous[ROUTE_ADDR_WIRE_SIZE] = {0}, socket_id;
        while (node_udp_endpoint(detector, peer, remote->ss_family, NAT_ENDPOINT_DIRECT, previous, &candidate, &socket_id, NULL)) {
            if (route_ping_addr_encode(previous, &candidate)) break;
            if (!memcmp(previous, destination, sizeof(previous))) target = 1;
            if (route_ping_addr_equal(&candidate, &other->remote_addr, 1)) anchor = 1;
            if (target && anchor) break;
        }
        if (target && anchor) break;
    }
    if (!target || !anchor) return 0;
    if (!state) state = socket_state(detector, socket, 1);
    if (!state) return 0;

    uint64_t now = get_time_tb();
    if (connection->links_up && signed_peer) {
        nat_detection_probe_delay(detector);
        for (struct ll_entry *entry = detector->instance->topo_groups->group_list->head; entry; entry = entry->next) {
            const struct TOPO_GROUP *group = (const struct TOPO_GROUP *)entry;
            if (!control_route(detector, group->group_id, peer->ed25519_public_key)) continue;
            if (state->cold_until > until) until = state->cold_until;
            break;
        }
    }
    if (now >= until) return 0;
    DEBUG_TRACE(DEBUG_CATEGORY_NAT, "NAT reserves alternate IP before INIT socket=%s target=%s until_tb=%llu",
                socket->name, sockaddr_storage_to_str(remote).str, (unsigned long long)until);
    uint64_t remaining = until - now;
    return remaining > NAT_TICK_TB ? NAT_TICK_TB : remaining;
}

uint64_t nat_detection_udp_init_delay(struct NAT_DETECTION *detector, const struct ETCP_LINK *link) {
    if (link->is_tcp || link->is_server || link->init_retry_count) return 0;
    return alternate_init_delay(detector, link->etcp, link->conn, &link->remote_addr, link->last_recv_local_time + NAT_LINK_BOOTSTRAP_TB);
}

int nat_detection_udp_init_allowed(struct NAT_DETECTION *detector, const struct ETCP_CONN *connection,
                                   struct ETCP_SOCKET *socket, const struct sockaddr_storage *source) {
    return !connection->links_up || !alternate_init_delay(detector, connection, socket, source, 0);
}

int nat_detection_ping_allowed(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
                               const uint8_t peer_key[32], const struct sockaddr_storage *source,
                               uint8_t flags, const uint8_t *tag, size_t length) {
    if (!detector || !socket) return 1;
    struct TOPO_GROUP *group = NULL;
    if ((flags & ETCP_PING_FLAG_OBSERVE) && tag && length >= ROUTE_OBSERVE_TAG_SIZE && !memcmp(tag, "NAT4UDP!", 8)) {
        group = topo_groups_find_wire(detector->instance->topo_groups, tag + ROUTE_OBSERVE_GROUP_OFF);
        if (!group || !group_node_x(detector, group->group_id, peer_key)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT observation denied group=%s transport=%s source=%s",
                sc_public_key_hex(tag + ROUTE_OBSERVE_GROUP_OFF, (char[65]){0}),
                sc_public_key_hex(peer_key, (char[65]){0}), sockaddr_storage_to_str(source).str); return 0;
        }
    }
    if (socket->probe_only || (flags & (ETCP_PING_FLAG_ADDRESS_CHECK | ETCP_PING_FLAG_NO_REPLY))) return 1;
    nat_detection_probe_delay(detector);
    struct nat_socket *state = socket_state(detector, socket, 0);
    if (!state || (!state->reservation_fingerprint && !state->available_fingerprint) || state->cold_until <= get_time_tb() ||
        (!detector->automatic && !state->phase && !state->forced) ||
        (!state->phase && !state->cycling && !state->forced && state->measured_profile.source == ADDRESS_SOURCE_ETCP &&
         state->measured_profile.mapping && state->measured_profile.inbound)) return 1;
    if (!(flags & ETCP_PING_FLAG_OBSERVE)) {
        history_load(state);
        if (contacted(state, source)) return 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT background PING reply deferred socket=%s transport_key=%s source=%s",
                    socket->name, sc_public_key_hex(peer_key, (char[65]){0}), sockaddr_storage_to_str(source).str);
        return 0;
    }
    history_load(state);
    if (contacted(state, source)) return 1; /* Уже известный IP не может испортить испытание незнакомого входа. */
    if (!group || length != ROUTE_OBSERVE_TAG_SIZE) return 1;
    uint64_t group_id = group->group_id, round = read64(tag + 17);
    const struct TOPO_NODE *node = group_node_x(detector, group_id, peer_key);
    if (!node) return 0;
    const uint8_t *owner = node->ed25519_public_key;
    if (state->phase && state->group_id == group_id && state->round == round &&
        (!memcmp(state->helper_key, peer_key, 32) || !memcmp(state->observer_key, peer_key, 32))) return 1;
    for (struct nat_binding *binding = detector->bindings; binding; binding = binding->next)
        if (binding->group_id == group_id && binding->round == round &&
            !memcmp(binding->helper, node->ed25519_public_key, 32) && binding->expires > get_time_tb()) {
            owner = binding->owner; break;
        }
    if (memcmp(owner, detector->instance->my_ed25519_pubkey, 32) <= 0) return 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT observation deferred for cold owner priority socket=%s group=%016llx requester=%s owner=%s",
                socket->name, (unsigned long long)group_id, sc_public_key_hex(node->ed25519_public_key, (char[65]){0}), sc_public_key_hex(owner, (char[65]){0}));
    return 0;
}

/* Изменения нескольких групп объединяются; известный полный ETCP-профиль не перепроверяется из-за нового соседа. */
static void topology_wake(void *arg) {
    struct NAT_DETECTION *detector = arg; detector->topology_wake = NULL;
    for (struct nat_socket *state = detector->sockets; state; state = state->next) {
        if (state->socket->is_tcp) continue;
        uint64_t fingerprint = candidate_fingerprint(state, 1);
        int changed = fingerprint != state->available_fingerprint; state->available_fingerprint = fingerprint;
        if (state->phase) {
            if (!control_route(detector, state->group_id, state->current_pair.observer) ||
                (!key_empty(state->current_pair.helper) && !control_route(detector, state->group_id, state->current_pair.helper)))
                owner_finish(state, NAT_REASON_CONTROL_LOST);
            continue;
        }
        if (!changed || !fingerprint || !detector->automatic || state->cycling) continue;
        if (state->measured_profile.source == ADDRESS_SOURCE_ETCP && state->measured_profile.mapping && state->measured_profile.inbound)
            continue;
        state->cursor = (struct nat_pair){0};
        state->due = get_time_tb() + 2500;
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT available candidates changed socket=%s; early measurement scheduled", state->socket->name);
    }
}

void nat_detection_topology_changed(struct NAT_DETECTION *detector) {
    if (!detector || detector->stopping) return;
    detector->reservation_dirty = 1;
    if (detector->topology_wake) return;
    detector->topology_wake = uasync_call_soon(detector->instance->ua, detector, topology_wake);
    if (!detector->topology_wake) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT topology wake scheduling failed");
}

void nat_detection_cancel_group(struct NAT_DETECTION *detector, uint64_t group_id) {
    if (!detector || detector->stopping) return;
    for (struct nat_socket *state = detector->sockets; state; state = state->next) {
        if (state->group_id != group_id || (!state->phase && !state->cycling)) continue;
        owner_finish(state, NAT_REASON_CANCELLED); state->forced = 0; state->group_id = 0; state->due = get_time_tb() + 2500;
    }
    struct nat_probe *probe = detector->probes;
    while (probe) {
        struct nat_probe *next = probe->next;
        if (probe->group_id == group_id) helper_remove(probe);
        probe = next;
    }
    struct nat_binding **cursor = &detector->bindings;
    while (*cursor) {
        struct nat_binding *binding = *cursor;
        if (binding->group_id != group_id) { cursor = &binding->next; continue; }
        *cursor = binding->next; u_free(binding);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT group tasks cancelled group=%016llx", (unsigned long long)group_id);
    nat_detection_topology_changed(detector);
}

void nat_detection_revalidate_members(struct NAT_DETECTION *detector, uint64_t group_id) {
    if (!detector || detector->stopping) return;
    for (struct nat_socket *state = detector->sockets; state; state = state->next) {
        if (state->group_id != group_id || (!state->phase && !state->cycling) ||
            participants_allowed(detector, group_id, state->current_pair.observer, state->current_pair.helper)) continue;
        owner_finish(state, NAT_REASON_CANCELLED); state->forced = 0;
    }
    struct nat_probe *probe = detector->probes;
    while (probe) {
        struct nat_probe *next = probe->next;
        if (probe->group_id == group_id &&
            !participants_allowed(detector, group_id, probe->message.owner, probe->message.observer)) {
            DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT helper revoked group=%016llx owner=%s round=%016llx",
                       (unsigned long long)group_id, sc_public_key_hex(probe->message.owner, (char[65]){0}),
                       (unsigned long long)read64(probe->message.round));
            helper_remove(probe);
        }
        probe = next;
    }
    struct nat_binding **cursor = &detector->bindings;
    while (*cursor) {
        struct nat_binding *binding = *cursor;
        if (binding->group_id != group_id || participants_allowed(detector, group_id, binding->owner, binding->helper)) {
            cursor = &binding->next; continue;
        }
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT observation revoked group=%016llx owner=%s round=%016llx",
                   (unsigned long long)group_id, sc_public_key_hex(binding->owner, (char[65]){0}), (unsigned long long)binding->round);
        *cursor = binding->next; u_free(binding);
    }
    nat_detection_topology_changed(detector);
}

void nat_detection_cancel_for_conn(struct NAT_DETECTION *detector, struct ETCP_CONN *connection) {
    if (!detector || !connection || detector->stopping) return;
    nat_detection_topology_changed(detector);
}

/* Сопоставление с текущим подписанным snapshot не зависит от remote_socket_id до завершения INIT. */
void nat_detection_connection_failed(struct NAT_DETECTION *detector, struct ETCP_CONN *connection, int reverse_done) {
    if (!detector || detector->stopping || !detector->automatic || !connection || connection->links_up) return;
    const struct TOPO_NODE *node = topo_node_registry_lookup(detector->instance->topo_groups, connection->peer_node_id, connection->crypto_ctx.peer_public_key);
    if (!node || !node->timestamp || memcmp(node->public_key, connection->crypto_ctx.peer_public_key, 32)) return;
    for (struct ETCP_LINK *link = connection->links; link; link = link->next) {
        struct ETCP_SOCKET *socket = link->conn;
        if (!socket || link->is_tcp || !link->init_retry_count || link->link_status || socket->probe_only || socket->socks_udp ||
            socket->type == CFG_SERVER_TYPE_LOCAL || socket->type == CFG_SERVER_TYPE_PRIVATE) continue;
        const struct address_profile *profile = NULL; uint8_t target_socket = 0, config_type = CFG_SERVER_TYPE_UNKNOWN, type = 0;
        for (const struct TOPO_ADDR4 *a = node->v4_addrs; a; a = a->next) {
            const struct sockaddr_in *target = (const struct sockaddr_in *)&link->remote_addr;
            if (link->remote_addr.ss_family != AF_INET || !(a->protocol & TOPO_PROTO_UDP) ||
                a->port != ntohs(target->sin_port) || memcmp(a->addr, &target->sin_addr, 4)) continue;
            for (const struct TOPO_SOCKMETA4 *m = node->v4_sock_meta; m; m = m->next)
                if (m->id == a->socket_id) { profile = &m->address_profile; config_type = m->config_type; break; }
            target_socket = a->socket_id; type = a->type; break;
        }
        for (const struct TOPO_ADDR6 *a = node->v6_addrs; a; a = a->next) {
            const struct sockaddr_in6 *target = (const struct sockaddr_in6 *)&link->remote_addr;
            if (link->remote_addr.ss_family != AF_INET6 || !(a->protocol & TOPO_PROTO_UDP) ||
                a->port != ntohs(target->sin6_port) || memcmp(a->addr, &target->sin6_addr, 16)) continue;
            for (const struct TOPO_SOCKMETA6 *m = node->v6_sock_meta; m; m = m->next)
                if (m->id == a->socket_id) { profile = &m->address_profile; config_type = m->config_type; break; }
            target_socket = a->socket_id; type = a->type; break;
        }
        uint8_t target_class = sock_target_class(type, config_type, profile, public_address(detector, &link->remote_addr));
        if (target_class != SOCK_TC_PUB && !(reverse_done && target_class == SOCK_TC_CONTACT)) continue;
        struct nat_socket *state = socket_state(detector, socket, 1);
        if (!schedule_failure_recheck(state, node->ed25519_public_key, 0)) continue;
        for (struct ll_entry *e = detector->instance->topo_groups->group_list->head; e; e = e->next) {
            struct TOPO_GROUP *group = (struct TOPO_GROUP *)e;
            if (!control_route(detector, group->group_id, node->ed25519_public_key)) continue;
            struct nat_recheck_wire request = {.cmd = ETCP_RT_ID_NAT_DETECTION, .command = NAT_RECHECK,
                .version = NAT_WIRE_VERSION, .socket_id = target_socket};
            if (route_ping_addr_encode(request.address, &link->remote_addr)) break;
            struct ll_entry *packet = ll_alloc_lldgram(1 + sizeof(request));
            if (!packet) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT failure recheck allocation failed"); break; }
            packet->dgram[0] = ETCP_RT_ID_NAT_DETECTION; memcpy(packet->dgram + 1, &request, sizeof(request));
            packet->len = 1 + sizeof(request);
            if (etcp_route_send_key(detector->instance, group->group_id, node->ed25519_public_key, packet, 0,
                                ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT))
                DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT failure recheck forwarding failed group=%016llx peer=%s",
                           (unsigned long long)group->group_id, sc_public_key_hex(node->ed25519_public_key, (char[65]){0}));
            else DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT failure recheck sent group=%016llx peer=%s endpoint=%s",
                            (unsigned long long)group->group_id, sc_public_key_hex(node->ed25519_public_key, (char[65]){0}),
                            sockaddr_storage_to_str(&link->remote_addr).str);
            break;
        }
    }
}

static void connection_status(struct ETCP_CONN *connection, int status, void *arg) {
    struct NAT_DETECTION *detector = arg;
    if (status == ETCP_CONN_STATUS_DOWN || status == ETCP_CONN_STATUS_DELETE || status == ETCP_CONN_STATUS_REINIT)
        nat_detection_cancel_for_conn(detector, connection);
    else if (status == ETCP_CONN_STATUS_UP) nat_detection_topology_changed(detector);
}

/* Принятый вход с двумя публичными IP сразу подтверждает direct/open именно у принимающего рабочего сокета. */
void nat_detection_link_ready(struct NAT_DETECTION *detector, struct ETCP_LINK *link) {
    if (!detector || !link || !link->conn || link->conn->probe_only || detector->stopping) return;
    struct nat_socket *state = socket_state(detector, link->conn, 1);
    if (!state) return;
    if (!state->phase && !state->completed_tb) state->due = get_time_tb() + 2500;
    if (!link->is_server || !link->initialized || link->conn->socks_udp ||
        link->conn->type == CFG_SERVER_TYPE_PRIVATE || link->conn->type == CFG_SERVER_TYPE_LOCAL) return;
    const struct sockaddr_storage *local = link->is_tcp ? stcp_link_get_local_addr(link->tcp_link) : &state->local;
    if (!local) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT incoming connection cannot read local address socket=%s", link->conn->name); return;
    }
    if (local->ss_family != link->remote_addr.ss_family || !public_address(detector, local) ||
        !public_address(detector, &link->remote_addr) || !route_ping_addr_equal(local, &state->local, 0)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT incoming connection ignored socket=%s protocol=%s local=%s peer=%s interface=%s"
                    " reason=non_public_or_other_interface", link->conn->name, link->is_tcp ? "TCP" : "UDP",
                    sockaddr_storage_to_str(local).str, sockaddr_storage_to_str(&link->remote_addr).str,
                    sockaddr_storage_to_str(&state->local).str); return;
    }
    if (state->measured_profile.mapping == ADDRESS_MAP_REAL_IP && state->measured_profile.inbound == ADDRESS_IN_OPEN &&
        !state->phase && !state->cycling && !state->forced) return;
    state->measured_profile = (struct address_profile){ADDRESS_MAP_REAL_IP, ADDRESS_IN_OPEN, ADDRESS_SOURCE_ETCP};
    state->measured_address = state->local;
    memset(&state->observed_address, 0, sizeof(state->observed_address));
    if (link->is_tcp) {
        state->observed_address = *local;
        if (local->ss_family == AF_INET) ((struct sockaddr_in *)&state->observed_address)->sin_port = 0;
        else ((struct sockaddr_in6 *)&state->observed_address)->sin6_port = 0;
    }
    state->completed_tb = get_time_tb(); state->forced = 0;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT incoming connection confirms DIRECT/OPEN socket=%s id=%u generation=%016llx"
               " protocol=%s local=%s peer=%s evidence=accepted_connection", link->conn->name, link->conn->sock_id,
               (unsigned long long)link->conn->generation, link->is_tcp ? "TCP" : "UDP",
               sockaddr_storage_to_str(local).str, sockaddr_storage_to_str(&link->remote_addr).str);
    owner_finish(state, NAT_REASON_NONE);
    if (link->is_tcp) state->due = UINT64_MAX;
}

/* Одно TCP-наблюдение сравнивает только IP; нулевой порт не обещает доступный listener. */
void nat_detection_tcp_observed(struct NAT_DETECTION *detector, struct ETCP_LINK *link,
                                 const struct sockaddr_storage *local, const struct sockaddr_storage *observed) {
    if (!detector || detector->stopping || !link || !link->is_tcp || !link->conn || !local || !observed) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "TCP NAT observation has invalid socket/addresses"); return;
    }
    if (local->ss_family != observed->ss_family || !public_address(detector, observed) ||
        !public_address(detector, &link->remote_addr)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "TCP NAT observation ignored socket=%s local=%s observed=%s peer=%s reason=non_public_or_family",
                    link->conn->name, sockaddr_storage_to_str(local).str, sockaddr_storage_to_str(observed).str,
                    sockaddr_storage_to_str(&link->remote_addr).str); return;
    }
    struct nat_socket *state = socket_state(detector, link->conn, 1);
    if (!state) return;
    uint8_t mapping = public_address(detector, local) && route_ping_addr_equal(local, observed, 0) ?
        ADDRESS_MAP_REAL_IP : ADDRESS_MAP_UNKNOWN;
    struct sockaddr_storage address = *observed;
    if (address.ss_family == AF_INET) ((struct sockaddr_in *)&address)->sin_port = 0;
    else ((struct sockaddr_in6 *)&address)->sin6_port = 0;
    if (state->measured_profile.source == ADDRESS_SOURCE_ETCP && state->measured_profile.mapping == mapping &&
        route_ping_addr_equal(&state->observed_address, &address, 1)) return;
    uint8_t inbound = mapping == ADDRESS_MAP_REAL_IP && state->measured_profile.mapping == mapping &&
        route_ping_addr_equal(&state->observed_address, &address, 0) ?
        state->measured_profile.inbound : ADDRESS_IN_UNKNOWN;
    state->measured_profile = (struct address_profile){.mapping = mapping, .inbound = inbound, .source = ADDRESS_SOURCE_ETCP};
    state->observed_address = address; state->completed_tb = get_time_tb(); state->due = UINT64_MAX; state->reason = NAT_REASON_NONE;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "TCP NAT presence socket=%s local=%s observed=%s NAT=%s type=UNKNOWN inbound=%s",
               link->conn->name, sockaddr_storage_to_str(local).str, sockaddr_storage_to_str(&address).str,
               mapping == ADDRESS_MAP_REAL_IP ? "no" : "yes", address_inbound_name(inbound));
    select_profile(state);
}

void nat_detection_observed_by_peer(struct NAT_DETECTION *detector, struct ETCP_LINK *link,
                                    const struct sockaddr_storage *observed) {
    if (!detector || !link || !link->conn || link->is_tcp || link->conn->socks_udp || !observed->ss_family) return;
    if (!public_address(detector, observed)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT ignores LAN observation socket=%s peer=%s observed=%s",
                    link->conn->name, sc_public_key_hex(link->etcp->peer_ed25519_pubkey, (char[65]){0}), sockaddr_storage_to_str(observed).str); return;
    }
    struct nat_socket *state = socket_state(detector, link->conn, 1);
    if (!state) return;
    if ((state->measured_profile.mapping == ADDRESS_MAP_REAL_IP &&
         !route_ping_addr_equal(&state->local, observed, 1)) ||
        (state->measured_profile.mapping == ADDRESS_MAP_EIM &&
         !route_ping_addr_equal(&state->measured_address, observed, 1))) {
        if (state->phase) owner_finish(state, NAT_REASON_ADDRESS_CHANGED);
        clear_profile(state, NAT_REASON_ADDRESS_CHANGED);
        state->due = get_time_tb() + 2500;
    }
    if (link->conn->type != CFG_SERVER_TYPE_LOCAL && link->conn->type != CFG_SERVER_TYPE_PRIVATE) remember_address(state, observed);
}

void nat_detection_socket_removed(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket) {
    if (!detector || !socket || detector->stopping) return;
    for (struct nat_socket *state = detector->sockets; state; state = state->next) {
        if (state->helper_primary != socket) continue;
        state->helper_primary = NULL;
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT isolated helper removed source=%s helper=%s generation=%016llx",
                    state->socket->name, socket->name, (unsigned long long)socket->generation);
    }
    if (!socket->probe_only) { detector->publish_pending = 1; detector->publish_after = get_time_tb(); }
    for (struct nat_binding *binding = detector->bindings; binding; binding = binding->next)
        if (binding->observer_socket == socket->sock_id && binding->observer_generation == socket->generation) binding->expires = 0;
    struct nat_probe *probe = detector->probes;
    while (probe) {
        struct nat_probe *next = probe->next;
        int observer_removed = probe->observer_socket == socket->sock_id && probe->observer_generation == socket->generation;
        int source_removed = probe->source_socket == socket->sock_id && probe->source_generation == socket->generation;
        if (probe->observer_generation && (observer_removed || source_removed)) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT public UDP pair socket removed socket=%s round=%016llx",
                       socket->name, (unsigned long long)read64(probe->message.round));
            helper_finish(probe, NAT_REASON_NO_HELPER);
        } else if (source_removed || (probe->io_socket == socket->sock_id && probe->io_generation == socket->generation)) helper_remove(probe);
        probe = next;
    }
    struct nat_socket **cursor = &detector->sockets;
    while (*cursor && (*cursor)->socket != socket) cursor = &(*cursor)->next;
    if (!*cursor) return;
    struct nat_socket *state = *cursor; *cursor = state->next;
    etcp_cancel_pings(detector->instance, owner_ping_cb, state);
    etcp_cancel_pings(detector->instance, owner_heartbeat_cb, state);
    struct ETCP_SOCKET *primary = state->helper_primary;
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT endpoint removed socket=%s generation=%016llx", socket->name,
                (unsigned long long)socket->generation);
    u_free(state);
    if (primary) etcp_socket_remove(primary);
}

static void socket_changed(struct ETCP_SOCKET *socket, int event, void *arg) {
    struct NAT_DETECTION *detector = arg;
    struct nat_socket *state = socket_state(detector, socket, 0);
    if (!state || !(event & ETCP_SOCKET_EVENT_ADDR_CHANGED)) return;
    uint64_t generation;
    if (random_id(&generation)) return;
    clear_profile(state, NAT_REASON_ADDRESS_CHANGED);
    nat_detection_socket_removed(detector, socket);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "Address change on %s: generation=%llu->%llu transport=%s", socket->name,
               (unsigned long long)socket->generation, (unsigned long long)generation,
               socket->is_tcp ? "tcp" : socket->transport ? "udp-worker" : "udp-direct");
    socket->generation = generation;
    if (socket->transport) packet_transport_set_generation(socket->transport, generation);
    socket_state(detector, socket, 1);
}

struct NAT_DETECTION *nat_detection_create(struct UTUN_INSTANCE *instance) {
    if (!instance || !instance->topo_groups || !instance->router_conns ||
        instance->router_bindings.callbacks[ETCP_RT_ID_NAT_DETECTION]) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT detector invalid instance/binding collision"); return NULL;
    }
    struct NAT_DETECTION *detector = u_calloc(1, sizeof(*detector));
    if (!detector) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT detector allocation failed"); return NULL; }
    detector->instance = instance; detector->automatic = 1; detector->reservation_dirty = 1; detector->history_refs = 1;
    if (etcp_router_bind(instance, ETCP_RT_ID_NAT_DETECTION, receive_control)) { u_free(detector); return NULL; }
    if (etcp_router_set_chat_policy(instance, ETCP_RT_ID_NAT_DETECTION,
                                   ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT CHAT router policy initialization failed"); goto fail;
    }
    etcp_add_conn_status_cbk(instance, connection_status, detector);
    if (!instance->conn_status_cbks || instance->conn_status_cbks->fn != connection_status || instance->conn_status_cbks->arg != detector) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT connection subscription allocation failed"); goto fail;
    }
    etcp_add_socket_cbk(instance, socket_changed, detector, ETCP_SOCKET_EVENT_ADDR_CHANGED);
    if (!instance->socket_cbks || instance->socket_cbks->fn != socket_changed || instance->socket_cbks->arg != detector) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT socket subscription allocation failed"); goto fail;
    }
    detector->timer = uasync_set_timeout(instance->ua, NAT_TICK_TB, detector, detector_tick, "nat_detection");
    if (!detector->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT detector initial timer failed"); goto fail;
    }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT endpoint detector started: UDP mapping/inbound, TCP IP-only presence with port=0");
    return detector;
fail:
    nat_detection_destroy(detector); return NULL;
}

void nat_detection_destroy(struct NAT_DETECTION *detector) {
    if (!detector) return;
    detector->stopping = 1;
    if (detector->timer) uasync_cancel_timeout(detector->instance->ua, detector->timer);
    if (detector->topology_wake) uasync_call_soon_cancel(detector->instance->ua, detector->topology_wake);
    etcp_remove_conn_status_cbk(detector->instance, connection_status, detector);
    etcp_remove_socket_cbk(detector->instance, socket_changed, detector);
    etcp_router_unbind(detector->instance, ETCP_RT_ID_NAT_DETECTION);
    while (detector->probes) helper_remove(detector->probes);
    while (detector->sockets) {
        struct nat_socket *state = detector->sockets; detector->sockets = state->next;
        etcp_cancel_pings(detector->instance, owner_ping_cb, state);
        etcp_cancel_pings(detector->instance, owner_heartbeat_cb, state);
        struct ETCP_SOCKET *primary = state->helper_primary; u_free(state);
        if (primary) etcp_socket_remove(primary);
    }
    while (detector->bindings) {
        struct nat_binding *binding = detector->bindings; detector->bindings = binding->next; u_free(binding);
    }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT detector stopped; all endpoint rounds and helper probes cancelled");
    if (detector->history_refs > 1) DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT context retained for %u database callbacks", detector->history_refs - 1);
    history_release(detector);
}

void nat_detection_request_check_all(struct NAT_DETECTION *detector) {
    if (!detector || detector->stopping) return;
    for (struct ETCP_SOCKET *socket = detector->instance->etcp_sockets; socket; socket = socket->next) {
        struct nat_socket *state = socket_state(detector, socket, 1);
        if (!state) continue;
        if (state->phase) owner_finish(state, NAT_REASON_CANCELLED);
        state->cycling = 0; state->cursor = (struct nat_pair){0};
        state->due = get_time_tb(); state->forced = 1; memset(state->preferred_observer, 0, 32);
    }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT endpoint recheck requested");
}

int nat_detection_request_check_socket(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket, const uint8_t observer[32]) {
    uint8_t transport_key[32];
    if (!observer || sc_identity_public_x("requested NAT observer", observer, transport_key) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT endpoint recheck has invalid observer key"); return -1;
    }
    struct nat_socket *state = socket_state(detector, socket, 1);
    if (!state) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT endpoint recheck has invalid socket"); return -1;
    }
    if (state->phase) owner_finish(state, NAT_REASON_CANCELLED);
    state->cycling = 0; state->cursor = (struct nat_pair){0};
    memcpy(state->preferred_observer, observer, 32); state->due = get_time_tb(); state->forced = 1;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT endpoint recheck socket=%s observer=%s", socket->name, sc_public_key_hex(observer, (char[65]){0}));
    return 0;
}

void nat_detection_set_allow_local(struct NAT_DETECTION *detector, int allow) {
    if (detector) detector->allow_local = !!allow;
}

void nat_detection_set_automatic(struct NAT_DETECTION *detector, int automatic) {
    if (!detector) return;
    if (automatic && !detector->automatic) {
        uint64_t now = get_time_tb();
        for (struct nat_socket *state = detector->sockets; state; state = state->next) {
            if (state->phase || state->cycling ||
                (state->measured_profile.source == ADDRESS_SOURCE_ETCP && state->measured_profile.mapping && state->measured_profile.inbound))
                continue;
            state->cold_until = now + NAT_ROUND_TB + 2500; state->due = now + 2500;
        }
    }
    detector->automatic = !!automatic;
    detector->reservation_dirty = 1;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT automatic endpoint measurements %s", automatic ? "enabled" : "disabled");
}

int nat_detection_get_status(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket, struct nat_check_status *status) {
    struct nat_socket *state = socket_state(detector, socket, 0);
    if (!state || !status) return -1;
    *status = (struct nat_check_status){.generation = socket->generation, .round = state->round, .group_id = state->group_id,
        .rounds = state->rounds, .helper_reselections = state->helper_reselections,
        .phase = state->phase, .reason = state->reason, .history_complete = state->history_complete, .cycling = state->cycling,
        .measured_profile = state->measured_profile, .effective_profile = socket->address_profile};
    memcpy(status->observer, state->observer_ed, 32);
    memcpy(status->helper, state->helper_ed, 32);
    return 0;
}
