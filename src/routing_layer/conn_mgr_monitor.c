/* conn_mgr_monitor.c — bg_ping, candidate_ping, idle, best_candidates */
#include "conn_mgr_priv.h"
#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "utun_instance.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_link_io.h"
#include "etcp_router.h"
#include "topo_node.h"
#include "topo_group.h"
#include "route_connectivity.h"
#include "route_ping.h"
#include "nat_detection.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif

/* ═══════ probe time утилиты ═══════ */

uint64_t cm_get_node_max_probe_time(struct TOPO_GROUP_NODE* nq) {
    uint64_t t = nq->connectivity.interface_probe_time;
    if (nq->connectivity.nat_probe_time > t) t = nq->connectivity.nat_probe_time;
    if (nq->connectivity.real_probe_time > t) t = nq->connectivity.real_probe_time;
    return t;
}

int cm_is_rtt_fresh(struct TOPO_GROUP_NODE* nq, uint64_t now_tb) {
    return cm_get_node_max_probe_time(nq) > 0 && (now_tb - cm_get_node_max_probe_time(nq)) < (uint64_t)(CACHE_MS * 10);
}

/* ═══════ best_candidates ═══════ */

/* Обновить RTT кандидата-посредника в отсортированном списке (≤3). Вызывается
 * из route_connectivity после успешного probe. INDIRECT-фаза использует этот
 * список (best_candidates) для выбора посредника с минимальным суммарным RTT. */
void conn_mgr_update_best_candidates(struct CONN_MGR* mgr, const uint8_t ed[32], uint16_t rtt) {
    if (!mgr || !ed || !memcmp(ed, mgr->instance->my_ed25519_pubkey, 32) || !rtt || rtt == UINT16_MAX) return;
    uint8_t slot = 0;
    while (slot < mgr->best_candidate_count && memcmp(mgr->best_candidates[slot].ed, ed, 32)) slot++;
    if (slot == mgr->best_candidate_count) {
        if (slot < CONN_MGR_MAX_CANDIDATES) mgr->best_candidate_count++;
        else {
            slot = CONN_MGR_MAX_CANDIDATES - 1;
            /* Быстрый, но недоступный кандидат не блокирует рабочий READY-путь. */
            int ready = cm_direct_conn_ed(mgr, ed) != NULL;
            int previous_ready = cm_direct_conn_ed(mgr, mgr->best_candidates[slot].ed) != NULL;
            for (int i = slot - 1; i >= 0 && previous_ready; i--)
                if (!cm_direct_conn_ed(mgr, mgr->best_candidates[i].ed)) { slot = (uint8_t)i; previous_ready = 0; }
            if (ready < previous_ready || (ready == previous_ready && (rtt > mgr->best_candidates[slot].rtt ||
                (rtt == mgr->best_candidates[slot].rtt && memcmp(ed, mgr->best_candidates[slot].ed, 32) >= 0)))) return;
        }
        char hex[65], previous[65];
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: candidate admitted peer=%s rtt=%u replacing=%s",
                    sc_public_key_hex(ed, hex), rtt, sc_public_key_hex(mgr->best_candidates[slot].ed, previous));
    }
    memcpy(mgr->best_candidates[slot].ed, ed, 32);
    mgr->best_candidates[slot].rtt = rtt;
    for (uint8_t i = 0; i < mgr->best_candidate_count; i++) for (uint8_t j = i + 1; j < mgr->best_candidate_count; j++) {
        struct CONN_MGR_CANDIDATE *a = &mgr->best_candidates[i], *b = &mgr->best_candidates[j];
        if (b->rtt < a->rtt || (b->rtt == a->rtt && memcmp(b->ed, a->ed, 32) < 0)) {
            struct CONN_MGR_CANDIDATE tmp = *a; *a = *b; *b = tmp;
        }
    }
}

/* Маршрут может вести через посредника. Для прямой пробы нужны transport peer и его живой линк. */
static struct ETCP_LINK* cm_active_direct_link(struct TOPO_GROUP* group, const struct TOPO_GROUP_NODE* node) {
    if (!node->identity) return NULL;
    struct ETCP_CONN* conn = instance_find_conn_by_key(group->instance, node->identity->public_key);
    if (!conn || memcmp(conn->peer_ed25519_pubkey, node->ed25519, 32) || conn->state != 1 || conn->close_requested) return NULL;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next)
        if (link->initialized && link->link_status) return link;
    return NULL;
}

/* ═══════ bg_ping ═══════ */

static void cm_bg_ping_noop_cb(int s, uint16_t r, void* a, uint64_t n, const uint8_t* d, size_t l) { (void)s;(void)r;(void)a;(void)n;(void)d;(void)l; }

/* Candidate TCP-пинг: по завершении (успех/ошибка/таймаут) зануляет свой слот в
 * mgr->cand_tcp_pings, чтобы conn_mgr_destroy не трогал уже освобождённый handle. */
static void cm_cand_tcp_ping_cb(int s, uint16_t r, void* arg, uint64_t n, const uint8_t* d, size_t l) {
    (void)s;(void)r;(void)n;(void)d;(void)l;
    struct stcp_client** slot = (struct stcp_client**)arg;
    if (slot) *slot = NULL;
}

static void cm_bg_ping_to_node(struct CONN_MGR* mgr, struct TOPO_NODE* ni) {
    for (const struct TOPO_ADDR4* a = ni->v4_addrs; a; a = a->next) {
        if (!(a->protocol & TOPO_PROTO_UDP)) continue;
        if (a->type != TOPO_ADDR_NAT && a->type != TOPO_ADDR_INTERFACE) continue;
        struct sockaddr_in sin; memset(&sin,0,sizeof(sin)); sin.sin_family=AF_INET;
        memcpy(&sin.sin_addr.s_addr,a->addr,4); sin.sin_port=htons(a->port);
        struct sockaddr_storage sa; memcpy(&sa,&sin,sizeof(sin));
        struct ETCP_SOCKET* s = mgr->instance->etcp_sockets;
        while (s) { if (!s->is_tcp && !s->probe_only && s->local_addr.ss_family == AF_INET) break; s = s->next; }
        if (!s) break;
        etcp_send_ping_to_socket(mgr->instance, s, ni->public_key, &sa, CONN_PROBE_TIMEOUT_MS, cm_bg_ping_noop_cb, NULL, NULL, 0, 0);
        return;
    }
    for (const struct TOPO_ADDR6* a6 = ni->v6_addrs; a6; a6 = a6->next) {
        if (!(a6->protocol & TOPO_PROTO_UDP)) continue;
        if (a6->type != TOPO_ADDR_NAT && a6->type != TOPO_ADDR_INTERFACE) continue;
        struct sockaddr_in6 sin6; memset(&sin6,0,sizeof(sin6)); sin6.sin6_family=AF_INET6;
        memcpy(&sin6.sin6_addr,a6->addr,16); sin6.sin6_port=htons(a6->port);
        struct ETCP_SOCKET* s = mgr->instance->etcp_sockets;
        while (s) { if (!s->is_tcp && !s->probe_only && s->local_addr.ss_family == AF_INET6) break; s = s->next; }
        if (!s) break;
        if (cm_classify_v6_addr(a6->addr) == CM_V6_LL) sin6.sin6_scope_id = s->netif_index;
        struct sockaddr_storage sa; memcpy(&sa,&sin6,sizeof(sin6));
        etcp_send_ping_to_socket(mgr->instance, s, ni->public_key, &sa, CONN_PROBE_TIMEOUT_MS, cm_bg_ping_noop_cb, NULL, NULL, 0, 0);
        return;
    }
}

/* Фоновый ping: один узел группы за тик (~100ms), полный цикл ≥ 60с.
 * Пингует только если нет активного ETCP-соединения с узлом. Цель: поддерживать
 * свежие RTT-замеры для ВСЕХ узлов группы — без этого best_candidates протухают
 * и INDIRECT-фаза не может выбрать посредника. */
void cm_bg_ping_timer_cb(void* arg) {
    struct CONN_MGR* mgr = (struct CONN_MGR*)arg;
    mgr->bg_ping_timer = NULL; mgr->bg_ping_wait = NULL;
    cm_collect(mgr);
    uint64_t nat_delay = nat_detection_probe_delay(mgr->instance->nat_det);
    if (nat_delay) {
        mgr->bg_ping_timer = uasync_set_timeout(mgr->instance->ua, nat_delay, mgr, cm_bg_ping_timer_cb, "cm_bg_nat_defer");
        if (!mgr->bg_ping_timer) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "background ping NAT defer scheduling failed");
        return;
    }
#ifdef UTUN_HAVE_STANDBY
    if (standby_get_sleep_tb() > 0) {
        mgr->bg_ping_wait = standby_wait(mgr, cm_bg_ping_timer_cb);
        if (mgr->bg_ping_wait) return;
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: background standby wait failed; retrying by timer"); goto rearm;
    }
    mgr->bg_ping_wait = NULL;
#endif
    struct TOPO_GROUP* group = mgr->group;
    if (!group || !group->nodes) goto rearm;
    size_t total = queue_entry_count(group->nodes);
    if (!total) goto rearm;

    uint64_t now = get_time_tb();
    if (mgr->bg_ping_cursor >= total) {
        mgr->bg_ping_cursor = 0;
        if ((now - mgr->bg_ping_cycle_start_tb) < CONN_MGR_BG_PING_CYCLE_MIN_TB)
        {
            mgr->bg_ping_timer = uasync_set_timeout(mgr->instance->ua,
                (int)(CONN_MGR_BG_PING_CYCLE_MIN_TB - (now - mgr->bg_ping_cycle_start_tb)), mgr, cm_bg_ping_timer_cb, "conn_mgr_bg_ping");
            if (!mgr->bg_ping_timer) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: background cycle timer allocation failed");
            return;
        }
        mgr->bg_ping_cycle_start_tb = now;
    }

    struct ll_entry* e = group->nodes->head;
    for (size_t c = 0; e && c < mgr->bg_ping_cursor; c++) e = e->next;
    if (e) {
        struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)e;
        if (nq != group->local_node && nq->identity && !cm_active_direct_link(group, nq)) cm_bg_ping_to_node(mgr, nq->identity);
        mgr->bg_ping_cursor++;
    }
rearm:
    mgr->bg_ping_timer = uasync_set_timeout(mgr->instance->ua, CONN_MGR_BG_PING_INTERVAL_TB, mgr, cm_bg_ping_timer_cb, "conn_mgr_bg_ping");
    if (!mgr->bg_ping_timer) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: background timer allocation failed");
}

/* ═══════ candidate_ping ═══════ */

/* Обновление кандидатов-посредников: удаляет протухших (>30с без свежих RTT),
 * активных пингует напрямую, неактивных probe'ит через route_connectivity.
 * Вызывается раз в ~2с, самоперезапускается. */
void cm_candidate_ping_timer_cb(void* arg) {
    struct CONN_MGR* mgr = (struct CONN_MGR*)arg; uint64_t now = get_time_tb();
    mgr->candidate_ping_timer = NULL; mgr->candidate_ping_wait = NULL;
    cm_collect(mgr);
    uint64_t nat_delay = nat_detection_probe_delay(mgr->instance->nat_det);
    if (nat_delay) {
        mgr->candidate_ping_timer = uasync_set_timeout(mgr->instance->ua, nat_delay, mgr, cm_candidate_ping_timer_cb, "cm_candidate_nat_defer");
        if (!mgr->candidate_ping_timer) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "candidate ping NAT defer scheduling failed");
        return;
    }
#ifdef UTUN_HAVE_STANDBY
    if (standby_get_sleep_tb() > 0) {
        mgr->candidate_ping_wait = standby_wait(mgr, cm_candidate_ping_timer_cb);
        if (mgr->candidate_ping_wait) return;
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: candidate standby wait failed; retrying by timer"); goto rearm;
    }
    mgr->candidate_ping_wait = NULL;
#endif
    /* Кандидаты-посредники выводятся из topo_node-списка группы: добавляем все
     * узлы с активным соединением (посредник должен быть напрямую связан с нами). */
    {
        struct ll_entry* e = mgr->group->nodes ? mgr->group->nodes->head : NULL;
        while (e) {
            struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)e;
            if (nq != mgr->group->local_node && cm_active_direct_link(mgr->group, nq)) {
                uint16_t rtt = topo_get_chain_rtt(nq);
                if (rtt == 0 || rtt == 0xFFFF) rtt = 1;
                conn_mgr_update_best_candidates(mgr, nq->ed25519, rtt);
            }
            e = e->next;
        }
    }
    for (uint8_t i = 0; i < mgr->best_candidate_count; i++) {
        const uint8_t* ed = mgr->best_candidates[i].ed;
        struct TOPO_GROUP_NODE* nq = topo_node_find_by_ed(mgr->group, ed);
        if (!nq) { memmove(&mgr->best_candidates[i], &mgr->best_candidates[i+1], (--mgr->best_candidate_count - i) * sizeof(mgr->best_candidates[0])); i--; continue; }
        struct ETCP_LINK* link = cm_active_direct_link(mgr->group, nq);
        char ed_hex[65];
        if (!link
            && cm_get_node_max_probe_time(nq) > 0
            && (now - cm_get_node_max_probe_time(nq)) >= CONN_MGR_CANDIDATE_STALE_TB)
        { DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: candidate %s stale, removing", sc_public_key_hex(ed, ed_hex));
          memmove(&mgr->best_candidates[i], &mgr->best_candidates[i+1], (--mgr->best_candidate_count - i) * sizeof(mgr->best_candidates[0])); i--; continue; }
        if (link) {
            struct ETCP_CONN* dc = link->etcp;
            struct TOPO_NODE* ni = nq->identity;
            /* пир в SLEEP-фазе — прямое соединение живо, пинг будит его: пропускаем */
            if (ni && !dc->peer_sleep_phase) {
                /* BLE и другие пакетные драйверы живут через ETCP keepalive, без IP-проб.
                 * При нескольких транспортах ищем живой IP-путь, не исключая пира из кандидатов. */
                struct etcp_link_snapshot transport;
                for (; link; link = link->next) {
                    if (!link->initialized || !link->link_status || link->closing) continue;
                    if (etcp_link_get_snapshot(link, &transport)) continue;
                    if (!transport.has_ip) {
                        DEBUG_DEBUG(DEBUG_CATEGORY_BGP,
                            "candidate probe skipped: peer=%s link=%u transport=%s reason=non-IP; ETCP keepalive active",
                            sc_public_key_hex(ed, ed_hex), link->local_link_id, transport.protocol);
                        continue;
                    }
                    if (link->remote_addr.ss_family != AF_INET && link->remote_addr.ss_family != AF_INET6) {
                        DEBUG_WARN(DEBUG_CATEGORY_BGP, "candidate probe invalid IP address: peer=%s link=%u transport=%s family=%d",
                                   sc_public_key_hex(ed, ed_hex), link->local_link_id, transport.protocol, link->remote_addr.ss_family);
                        continue;
                    }
                    break;
                }
                if (!link) continue;
                DEBUG_TRACE(DEBUG_CATEGORY_BGP, "candidate probe: group=%016llx peer=%s link=%u transport=%s",
                            (unsigned long long)mgr->group->group_id, sc_public_key_hex(ed, ed_hex), link->local_link_id,
                            transport.protocol);
                if (link->is_tcp) {
                    struct stcp_client** slot = NULL;
                    for (uint8_t i = 0; i < CONN_MGR_MAX_CANDIDATES; i++)
                        if (!mgr->cand_tcp_pings[i]) { slot = &mgr->cand_tcp_pings[i]; break; }
                    if (slot) etcp_send_tcp_ping(mgr->instance, ni->public_key, &link->remote_addr, CONN_PROBE_TIMEOUT_MS, cm_cand_tcp_ping_cb, slot, slot, link->reality_set ? &link->reality : NULL);
                } else
                    etcp_send_ping(mgr->instance, ni->public_key, &link->remote_addr, CONN_PROBE_TIMEOUT_MS, cm_bg_ping_noop_cb, NULL, NULL, 0);
            }
        } else route_connectivity_probe_node(mgr->instance, mgr->group, nq);
    }
rearm:
    mgr->candidate_ping_timer = uasync_set_timeout(mgr->instance->ua, CONN_MGR_CANDIDATE_PING_TB, mgr, cm_candidate_ping_timer_cb, "conn_mgr_cand_ping");
    if (!mgr->candidate_ping_timer) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: candidate timer allocation failed");
}
