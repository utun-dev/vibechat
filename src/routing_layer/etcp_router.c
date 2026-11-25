// etcp_router.c — Сервисный слой маршрутизации поверх ETCP
// Упрощённый TCP: восстановление порядка (recv_q), дедупликация, ретрансмиты (inflight_q)
// ACK: периодическая отправка rx_seq (не чаще 10ms, ROUTER_ACK_INTERVAL_TB), idle-таймер для последнего seq
// Inflight контроль через send_q: при переполнении — очередь + retry-таймер, без дропов
// Подпись/шифрование — в route_crypto.c (encode при отправке, decode до изменения состояния приёма)
//
// Поток сервисной кодограммы:
//   in -> {send_q} -> {inflight_q / etcp_send} -> ... -> {incoming_q} -> {recv_q} -> сервис
//   ack возвращается по обратному пути; (pause) — backpressure при заполнении очередей.


/*
           /----- ack                                          ack <-- id
           |(pause)                                                     |
in ---> {Q} -> [src, etcp] --> ... --> [dst,etcp] -> {asm_q} -> {buf q} ---> out

*/

#include "etcp_router.h"
#include "route_crypto.h"
#include "pkt_normalizer.h"
#include "etcp.h"
#include "utun_instance.h"
#include "topo_group.h"
#include "topo_node_sqlite.h"
#include "../chat/group_policy.h"
#include "conn_mgr.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/ll_queue.h"
#include "../lib/u_async.h"
#include <limits.h>
#include <string.h>

// ====================================================================
// Внутренние forward declarations
// ====================================================================
static void router_ack_timer_cb(void* arg);
static void router_idle_ack_timer_cb(void* arg);
static void router_send_ack(struct ETCP_ROUTER_CONN* rconn);
static void router_schedule_ack(struct ETCP_ROUTER_CONN* rconn);
static void router_try_assembly(struct ETCP_ROUTER_CONN* rconn, struct ETCP_CONN* conn);
static int router_deliver(struct ETCP_ROUTER_CONN* rconn, struct ETCP_CONN* conn, const uint8_t* wire, size_t wire_len);
static void router_send_ctrl(struct ETCP_ROUTER_CONN* rconn, uint8_t flag_bits);
static void router_handshake_timer_cb(void* arg);
static void router_handshake_start(struct ETCP_ROUTER_CONN* rconn);
static void router_cancel_send_waiter(struct ETCP_ROUTER_CONN* rconn);
static struct ETCP_CONN* router_send_conn(struct ETCP_ROUTER_CONN* rconn);
static void router_send_kick(struct ETCP_ROUTER_CONN* rconn);
static void router_send_drain_cb(struct ll_queue* q, void* arg);
static void router_send_watchdog_cb(void* arg);
static void router_send_watchdog_arm(struct ETCP_ROUTER_CONN* rconn);
static void router_send_watchdog_disarm(struct ETCP_ROUTER_CONN* rconn);
static void router_no_route_retry_cb(void* arg);
static void router_send_close_to_service(struct ETCP_ROUTER_CONN* rconn);
static void router_close_and_notify(struct ETCP_ROUTER_CONN* rconn);
static void router_close_finalize(void* arg);
static void router_conn_free_queues(struct ETCP_ROUTER_CONN* rconn);
static int router_conn_reset(struct ETCP_ROUTER_CONN* rconn);
static void free_entry(struct ll_entry* e) { queue_dgram_free(e); queue_entry_free(e); }
static void router_handle_ack(struct UTUN_INSTANCE* inst, struct ETCP_ROUTER_CONN* rconn, uint32_t seq,
                              uint64_t src_node_id, uint16_t ack_ts, int receive_busy);
static void router_forward_transit(struct UTUN_INSTANCE* inst, struct ll_entry* entry, struct SVC_ROUTE_HDR* hdr);
static void router_handle_data_packet(struct ETCP_ROUTER_CONN* rconn, struct ETCP_CONN* conn, uint32_t seq, const uint8_t* wire, size_t wire_len);
static int router_retransmit_one(struct ETCP_ROUTER_CONN* rconn, struct ROUTER_INFLIGHT* inf);
static uint64_t router_retrans_rto(const struct ETCP_ROUTER_CONN* rconn);
static uint64_t router_retrans_interval(const struct ETCP_ROUTER_CONN* rconn);
static void router_retrans_schedule(struct ETCP_ROUTER_CONN* rconn);
static void router_retrans_timer_cb(void* arg);
static void router_incoming_q_cb(struct ll_queue* q, void* arg);
static void router_track_inflight_state(struct ETCP_ROUTER_CONN* rconn);
static uint32_t router_effective_max_inflight(struct ETCP_ROUTER_CONN* rconn);
static void router_update_inflight_limit(struct ETCP_ROUTER_CONN* rconn);
static void router_update_minrtt(struct ETCP_ROUTER_CONN* rconn);

/* Тип берём локально; неизвестная группа не может запросить незащищённый формат UTUN. */
static enum route_crypto_format router_crypto_format(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    int utun = group ? group->group_type == TOPO_GROUP_TYPE_UTUN : group_id == TOPO_GROUP_UTUN;
    return utun ? ROUTE_CRYPTO_UTUN : ROUTE_CRYPTO_CHAT;
}

/* Общий service ID может ограничивать только CHAT; неизвестная группа не получает обход проверки. */
static int router_policy_applies(struct UTUN_INSTANCE* inst, uint64_t group_id, uint8_t svc) {
    if (!inst->router_bindings.chat_policy[svc]) return 1;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    return !group || group->group_type != TOPO_GROUP_TYPE_UTUN;
}

/* Политика не изменяет router; отказ не закрывает канал из чужого стека отправки. */
static int router_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint8_t svc) {
    if (!router_policy_applies(inst, group, svc)) return 1;
    int (*authorize)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]) = inst->router_bindings.authorize[svc];
    if (!authorize || authorize(inst, group, peer) == 1) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router access denied: group=%016llx peer=%s svc=%u",
               (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), svc);
    return 0;
}

/* CHAT доверяет только составу своей группы; прежний источник ключей UTUN сохранён. */
static int router_peer_keys(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t peer, const uint8_t expected[32], uint8_t x[32], uint8_t ed[32]) {
    if (router_crypto_format(inst, group_id) == ROUTE_CRYPTO_UTUN) {
        if (sc_identity_public_x("SVC_ROUTE", expected, x) != SC_OK) return -1;
        struct TOPO_NODE* node = topo_node_registry_lookup(inst->topo_groups, peer, x);
        if (node && !memcmp(expected, node->ed25519_public_key, 32)) { memcpy(ed, expected, 32); return 0; }
    } else {
        const char* channel = topo_groups_channel(inst->topo_groups, group_id);
        if (sc_identity_public_x("CHAT router", expected, x) == SC_OK && group_policy_member_key_allowed(inst, channel, x, expected, 0)) {
            memcpy(ed, expected, 32); return 0;
        }
    }
    DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router keys unavailable group=%016llx peer=%016llx",
        (unsigned long long)group_id, (unsigned long long)peer); return -1;
}

static int router_group_address(struct UTUN_INSTANCE* inst, struct SVC_ROUTE_HDR* h) {
    if (h->group_id == TOPO_GROUP_UTUN) {
        topo_group_utun_wire_id(h->group_key, TOPO_GROUP_UTUN);
        return 0;
    }
    const uint8_t* key = topo_group_wire_id(topo_groups_find(inst->topo_groups, h->group_id));
    if (!key) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router group has no verified address group=%016llx svc=%u",
            (unsigned long long)h->group_id, h->svc_id); return -1;
    }
    memcpy(h->group_key, key, 32); return 0;
}

/* В очередях каноническое тело содержит полные адреса, за ним идут ключи пути и счётчик.
 * На соединение ETCP передаются фиксированные поля, src, dst, count, visited, тело. */
static void router_replace_packet(struct ll_entry* e, uint8_t* bytes, size_t size) {
    queue_dgram_free(e); e->dgram_pool = NULL; e->dgram_free_fn = NULL;
    e->dgram = bytes; e->len = size; e->memlen = size;
}

static int router_wire_control(const struct SVC_ROUTE_HDR* h, size_t body) {
    size_t overhead = (h->flags & ROUTER_FLAG_SIGNED ? SC_SIGN_SIZE : 0) +
        (h->flags & ROUTER_FLAG_ENCRYPTED ? ROUTER_ENCRYPTION_OVERHEAD : 0);
    return (h->flags & ~(ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) || body <= overhead;
}

/* Кодирует адреса по текущей таблице перед каждой физической отправкой. */
static int router_wire_send(struct ETCP_CONN* conn, struct ll_entry* e, int force_full) {
    struct SVC_ROUTE_HDR* h = (struct SVC_ROUTE_HDR*)e->dgram;
    const struct TOPO_GROUP* group = topo_groups_find(conn->instance->topo_groups, h->group_id);
    if (e->len <= SVC_ROUTE_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router encode: missing path bytes=%u", e->len); return -1;
    }
    uint8_t trailer = e->dgram[e->len - 1], count = trailer & 0x7f;
    force_full |= !!(trailer & 0x80);
    if (!count || count > ROUTER_MAX_VISITED || e->len < SVC_ROUTE_HDR_SIZE + count * SC_PUBKEY_SIZE + 1) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router encode: invalid path count=%u bytes=%u", count, e->len); return -1;
    }
    size_t body = e->len - SVC_ROUTE_HDR_SIZE - count * SC_PUBKEY_SIZE - 1;
    size_t capacity = ROUTER_FIXED_FIELDS + (count + 2u) * ROUTER_FULL_ADDRESS_SIZE + 1 + body;
    if (capacity > PKTNORM_MAX_DGRAM_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router encode: invalid path count=%u body=%zu", count, body); return -1;
    }
    uint8_t* wire = u_malloc(capacity);
    if (!wire) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router wire allocation failed size=%zu", capacity); return -1; }
    memcpy(wire, h, ROUTER_FIXED_FIELDS);
    size_t pos = ROUTER_FIXED_FIELDS;
    int control = router_wire_control(h, body);
    enum topo_address_format path = force_full || control ? TOPO_ADDRESS_FULL : TOPO_ADDRESS_COMPACT;
    enum topo_address_format endpoints = h->flags & ROUTER_FLAG_ENCRYPTED ? path : TOPO_ADDRESS_FULL;
    for (unsigned i = 0; i < count + 2u; i++) {
        if (i == 2) wire[pos++] = count;
        const uint8_t* ed = i == 0 ? h->src_ed : i == 1 ? h->dst_ed : e->dgram + SVC_ROUTE_HDR_SIZE + body + (i - 2u) * SC_PUBKEY_SIZE;
        int n = topo_address_write(wire + pos, capacity - pos, group, ed, i < 2 ? endpoints : path);
        if (n < 0) { u_free(wire); return -1; }
        pos += (size_t)n;
    }
    memcpy(wire + pos, e->dgram + SVC_ROUTE_HDR_SIZE, body);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "router wire TX group=%016llx hops=%u body=%zu bytes=%zu control=%d full_retry=%d",
                (unsigned long long)h->group_id, count, body, pos + body, control, force_full);
    router_replace_packet(e, wire, pos + body);
    return etcp_send(conn, e);
}

static int router_path_append(struct ll_entry* e, const uint8_t ed[SC_PUBKEY_SIZE], uint8_t count) {
    uint8_t full = count & 0x80; count &= 0x7f;
    size_t kept = e->len - (count ? 1u : 0u), size = kept + SC_PUBKEY_SIZE + 1;
    if (count >= ROUTER_MAX_VISITED || size > UINT16_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router path limit: count=%u len=%u", count, e->len); return -1;
    }
    uint8_t* wire = u_malloc(size);
    if (!wire) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router path allocation failed size=%zu", size); return -1; }
    memcpy(wire, e->dgram, kept); memcpy(wire + kept, ed, SC_PUBKEY_SIZE); wire[size - 1] = full | (count + 1);
    router_replace_packet(e, wire, size); return 0;
}

static int router_read_address(const uint8_t* data, size_t len, struct TOPO_GROUP* group, enum topo_address_format format,
                               uint8_t ed[SC_PUBKEY_SIZE], uint64_t* id) {
    struct topo_wire_address a;
    int n = topo_address_read(data, len, format, &a);
    if (n < 0) return -1;
    if (!a.ed) {
        struct TOPO_GROUP_NODE* node = topo_address_find(group, a.short31, NULL);
        if (!node) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router unresolved short address group=%016llx short=%08x",
                       (unsigned long long)(group ? group->group_id : 0), a.short31); return -1;
        }
        memcpy(ed, node->ed25519, SC_PUBKEY_SIZE);
        if (id) *id = node->node_id;
    } else {
        memcpy(ed, a.ed, SC_PUBKEY_SIZE);
        if (id) {
            uint8_t x[SC_PUBKEY_SIZE];
            if (sc_identity_public_x("router endpoint", ed, x) != SC_OK) return -1;
            *id = sc_derive_node_id_from_pubkey(x);
        }
    }
    return n;
}

/* Разобрать весь маршрут до выбора сервиса/создания канала. */
static size_t router_wire_read(struct ETCP_CONN* conn, struct ll_entry* e) {
    struct UTUN_INSTANCE* inst = conn->instance;
    struct SVC_ROUTE_HDR h = {0};
    if (e->len < ROUTER_FIXED_FIELDS || e->len > PKTNORM_MAX_DGRAM_SIZE) goto malformed;
    memcpy(&h, e->dgram, ROUTER_FIXED_FIELDS);
    if (h.cmd != ETCP_ID_SVC_ROUTE) goto malformed;
    struct TOPO_GROUP* group = topo_groups_find_wire(inst->topo_groups, h.group_key);
    uint8_t utun[32]; topo_group_utun_wire_id(utun, TOPO_GROUP_UTUN);
    if (group) h.group_id = group->group_id;
    else if (!memcmp(h.group_key, utun, 32)) h.group_id = TOPO_GROUP_UTUN;
    else goto malformed;
    size_t pos = ROUTER_FIXED_FIELDS;
    enum topo_address_format ep = h.flags & ROUTER_FLAG_ENCRYPTED ? TOPO_ADDRESS_COMPACT : TOPO_ADDRESS_FULL;
    uint64_t src, dst;
    int n = router_read_address(e->dgram + pos, e->len - pos, group, ep, h.src_ed, &src);
    if (n < 0) goto malformed;
    int compact = n == 4;
    pos += n;
    n = router_read_address(e->dgram + pos, e->len - pos, group, ep, h.dst_ed, &dst);
    if (n < 0) goto malformed;
    compact |= n == 4; pos += n;
    if (pos >= e->len) goto malformed;
    h.src_node_id = src; h.dst_node_id = dst;
    uint8_t count = e->dgram[pos++], visited[ROUTER_MAX_VISITED][SC_PUBKEY_SIZE];
    if (!count || count > ROUTER_MAX_VISITED) goto malformed;
    for (unsigned i = 0; i < count; i++) {
        n = router_read_address(e->dgram + pos, e->len - pos, group, TOPO_ADDRESS_COMPACT, visited[i], NULL);
        if (n < 0) goto malformed;
        compact |= n == 4; pos += n;
        for (unsigned j = 0; j < i; j++) if (!memcmp(visited[j], visited[i], SC_PUBKEY_SIZE)) goto malformed;
        if (!memcmp(visited[i], inst->my_ed25519_pubkey, SC_PUBKEY_SIZE)) {
            inst->router_loop_drops++;
            DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router loop group=%016llx peer=%016llx hop=%u count=%u",
                       (unsigned long long)h.group_id, (unsigned long long)conn->peer_node_id, i, count); return 0;
        }
    }
    size_t body = e->len - pos;
    if (memcmp(visited[0], h.src_ed, SC_PUBKEY_SIZE) ||
        sc_identity_check("router previous hop", conn->crypto_ctx.peer_public_key, visited[count-1]) != SC_OK ||
        body > ROUTER_MAX_ENCODED_BODY || (compact && router_wire_control(&h, body))) goto malformed;
    size_t head_body = SVC_ROUTE_HDR_SIZE + body, size = head_body + count * SC_PUBKEY_SIZE + 1;
    uint8_t* canonical = u_malloc(size);
    if (!canonical) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router decode allocation failed size=%zu", size); return 0; }
    memcpy(canonical, &h, sizeof(h)); memcpy(canonical + sizeof(h), e->dgram + pos, body);
    memcpy(canonical + head_body, visited, count * SC_PUBKEY_SIZE); canonical[size - 1] = compact ? count : (0x80 | count);
    router_replace_packet(e, canonical, size); return head_body;
malformed:
    inst->router_path_errors++;
    DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router malformed packet peer=%016llx group=%s bytes=%u failures=%llu",
               (unsigned long long)conn->peer_node_id, sc_public_key_hex(h.group_key, (char[65]){0}), e->len, (unsigned long long)inst->router_path_errors);
    return 0;
}

static int router_source_send(struct ETCP_CONN* conn, struct ll_entry* e, int retry) {
    struct UTUN_INSTANCE* inst = conn->instance;
    struct SVC_ROUTE_HDR* header = (struct SVC_ROUTE_HDR*)e->dgram;
    if (e->len == SVC_ROUTE_HDR_SIZE && router_group_address(inst, header)) return -1;
    if (!router_allowed(inst, header->group_id, header->dst_ed, header->svc_id)) return -1;
    enum route_crypto_format format = router_crypto_format(inst, header->group_id);
    int control_crypto = format == ROUTE_CRYPTO_CHAT ? ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT :
        router_policy_applies(inst, header->group_id, header->svc_id) ?
        inst->router_bindings.required_crypto[header->svc_id] & ROUTER_FLAG_SIGNED : 0;
    if (e->len == SVC_ROUTE_HDR_SIZE && control_crypto) {
        uint8_t* signed_wire = NULL; size_t size = 0;
        uint8_t x[32], ed[32];
        if ((control_crypto & ROUTE_CRYPTO_ENCRYPT) && router_peer_keys(inst, header->group_id, header->dst_node_id, header->dst_ed, x, ed)) return -1;
        if (route_crypto_encode(inst, format, control_crypto & ROUTE_CRYPTO_ENCRYPT ? x : NULL, e->dgram, e->len, control_crypto, &signed_wire, &size)) return -1;
        router_replace_packet(e, signed_wire, size);
    }
    if (router_path_append(e, inst->my_ed25519_pubkey, 0) < 0) return -1;
    const struct SVC_ROUTE_HDR* h = (const struct SVC_ROUTE_HDR*)e->dgram;
    DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, "route TX conn=%s svc=%u flags=%02x seq=%u len=%u src=%016llx dst=%016llx group=%016llx epoch=%016llx",
                conn->log_name, h->svc_id, h->flags, h->seq, e->len,
                (unsigned long long)h->src_node_id, (unsigned long long)h->dst_node_id,
                (unsigned long long)h->group_id, (unsigned long long)h->reset_id);
    return router_wire_send(conn, e, retry);
}


static void router_cancel_send_waiter(struct ETCP_ROUTER_CONN* rconn) {
    if (rconn->send_waiter_q) queue_waiter_cancel(rconn->send_waiter_q, &rconn->send_waiter);
    rconn->send_waiter_q = NULL;
}

// ====================================================================
// Транзитные очереди — per (group_id, src_node_id, dst_node_id) pair, backpressure через waiter на send_input_q
// ====================================================================

// Найти транзитную очередь для пары (group_id, src, dst) на conn (NULL если не создана).
static struct TRANSIT_QUEUE* transit_queue_find(struct ETCP_CONN* conn,
                                                 const struct SVC_ROUTE_HDR* h) {
    if (!conn->transit_queues) return NULL;
    uint8_t key[8 + 2 * SC_PUBKEY_SIZE];
    memcpy(key, &h->group_id, 8);
    memcpy(key + 8, h->src_ed, 32);
    memcpy(key + 40, h->dst_ed, 32);
    return (struct TRANSIT_QUEUE*)queue_find_data_by_index(conn->transit_queues, key);
}

// Найти транзитную очередь, при отсутствии — создать (реестр транзитных очередей живёт на conn).
static struct TRANSIT_QUEUE* transit_queue_get_or_create(struct ETCP_CONN* conn,
                                                          const struct SVC_ROUTE_HDR* h) {
    if (!conn->transit_queues) {
        struct UASYNC* ua = conn->instance ? conn->instance->ua : NULL;
        if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "transit_queue_get_or_create: no ua"); return NULL; }
        conn->transit_queues = queue_new(ua, TRANSIT_QUEUE_HASH, 0, 8 + 2 * SC_PUBKEY_SIZE, "transit_q_reg");
        if (!conn->transit_queues) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "transit_queue_get_or_create: queue_new failed"); return NULL; }
    }
    struct TRANSIT_QUEUE* tq = transit_queue_find(conn, h);
    if (tq) return tq;
    size_t data_size = sizeof(struct TRANSIT_QUEUE) - sizeof(struct ll_entry);
    tq = (struct TRANSIT_QUEUE*)queue_entry_new(data_size);
    if (!tq) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "transit_queue_get_or_create: queue_entry_new failed");
        return NULL;
    }
    memset(&tq->waiter, 0, sizeof(tq->waiter));
    tq->group_id = h->group_id;
    tq->src_node_id = h->src_node_id;
    tq->dst_node_id = h->dst_node_id;
    tq->conn = conn;
    tq->q = queue_new(conn->instance->ua, 0, 0, 0, "transit_pair_q");
    if (!tq->q) { queue_entry_free(&tq->ll); return NULL; }
    memcpy(tq->src_ed, h->src_ed, 32); memcpy(tq->dst_ed, h->dst_ed, 32);
    queue_data_put_with_index(conn->transit_queues, &tq->ll);
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "transit_q: new group=%016llx src=%016llx dst=%016llx",
               (unsigned long long)h->group_id, (unsigned long long)h->src_node_id, (unsigned long long)h->dst_node_id);
    return tq;
}

// Уничтожить транзитную очередь: снять waiter, дропнуть оставшиеся пакеты, удалить из реестра conn.
static void transit_queue_destroy(struct ETCP_CONN* conn, struct TRANSIT_QUEUE* tq) {
    if (tq->route_timer) uasync_cancel_timeout(conn->instance->ua, tq->route_timer);
    if (conn->send_input_q) queue_waiter_cancel(conn->send_input_q, &tq->waiter);
    int dropped = 0;
    struct ll_entry* e;
    while ((e = queue_data_get(tq->q)) != NULL) { free_entry(e); dropped++; }
    if (dropped > 0)
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "transit_q: destroyed with %d pending pkts src=%016llx dst=%016llx",
                   dropped, (unsigned long long)tq->src_node_id, (unsigned long long)tq->dst_node_id);
    queue_free(tq->q);
    queue_remove_data(conn->transit_queues, &tq->ll);
    queue_entry_free(&tq->ll);
}

// Waiter-коллбэк транзитной очереди: send_input_q освободился → шлём один пакет,
// при непустой очереди снова встаём в waiter, иначе уничтожаем очередь.
static void transit_queue_drain_cb(struct ll_queue* q, void* arg);
static void transit_queue_retry(void* arg) {
    struct TRANSIT_QUEUE* tq = arg;
    tq->route_timer = NULL;
    transit_queue_drain_cb(tq->conn->send_input_q, tq);
}

static void transit_queue_wait(struct TRANSIT_QUEUE* tq) {
    if (!tq->route_timer)
        tq->route_timer = uasync_set_timeout(tq->conn->instance->ua, ROUTER_NO_ROUTE_RETRY_TB, tq, transit_queue_retry, "transit_route");
    int waiting = tq->waiter.internal || tq->waiter.call_soon_id;
    if (!tq->route_timer || (!waiting && queue_waiter_wait(tq->conn->send_input_q, &tq->waiter, transit_queue_drain_cb, tq) < 0)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "transit wait failed dst=%016llx", (unsigned long long)tq->dst_node_id);
        transit_queue_destroy(tq->conn, tq);
    }
}

static struct ETCP_CONN* router_group_next(struct TOPO_GROUP* group, const uint8_t ed[SC_PUBKEY_SIZE]) {
    struct TOPO_NODEPATH* path = topo_node_best_path(topo_node_find_by_ed(group, ed));
    return path ? path->conn : NULL;
}

static void transit_queue_drain_cb(struct ll_queue* q, void* arg) {
    struct TRANSIT_QUEUE* tq = (struct TRANSIT_QUEUE*)arg;
    struct ETCP_CONN* conn = tq->conn;
    struct TOPO_GROUP* group = topo_groups_find(conn->instance->topo_groups, tq->group_id);
    struct ETCP_CONN* next = router_group_next(group, tq->dst_ed);
    if (next == conn && queue_entry_count(q)) { transit_queue_wait(tq); return; }
    queue_waiter_cancel(conn->send_input_q, &tq->waiter); /* cancel only when sending or switching the route */
    struct ll_entry* e = queue_data_get(tq->q);
    if (!e) { transit_queue_destroy(conn, tq); return; }
    int ret = 0;
    if (next != conn) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "transit reroute: dst=%016llx old=%016llx new=%016llx",
                    (unsigned long long)tq->dst_node_id, (unsigned long long)conn->peer_node_id,
                    (unsigned long long)(next ? next->peer_node_id : 0));
        router_forward_transit(conn->instance, e, (struct SVC_ROUTE_HDR*)e->dgram);
    } else ret = router_wire_send(conn, e, 0);
    if (ret != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "transit_drain: etcp_send failed ret=%d src=%016llx dst=%016llx",
                    ret, (unsigned long long)tq->src_node_id, (unsigned long long)tq->dst_node_id);
        free_entry(e);
    }
    if (queue_entry_count(tq->q) > 0)
        transit_queue_wait(tq);
    else
        transit_queue_destroy(conn, tq);
}

// Удалить все транзитные очереди соединения (вызывается при закрытии ETCP_CONN).
void etcp_router_transit_queues_destroy(struct ETCP_CONN* conn) {
    if (!conn || !conn->transit_queues) return;
    int pairs = 0, dropped = 0;
    struct ll_entry* entry;
    while ((entry = queue_data_get(conn->transit_queues)) != NULL) {
        struct TRANSIT_QUEUE* tq = (struct TRANSIT_QUEUE*)entry;
        if (tq->route_timer) uasync_cancel_timeout(conn->instance->ua, tq->route_timer);
        if (conn->send_input_q) queue_waiter_cancel(conn->send_input_q, &tq->waiter);
        struct ll_entry* e;
        while ((e = queue_data_get(tq->q)) != NULL) { free_entry(e); dropped++; }
        queue_free(tq->q);
        queue_entry_free(&tq->ll);
        pairs++;
    }
    queue_free(conn->transit_queues);
    conn->transit_queues = NULL;
    if (pairs) DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "transit queues released: peer=%016llx pairs=%d discarded=%d",
                           (unsigned long long)conn->peer_node_id, pairs, dropped);
}

// Сбросить recv_conn у всех ROUTER_CONN, ссылающихся на уничтожаемый ETCP_CONN.
// recv_conn — заимствованный указатель; без очистки он висит после u_free(conn) (UAF).
void etcp_router_conn_destroyed(struct ETCP_CONN* conn) {
    if (!conn || !conn->instance || !conn->instance->router_conns) return;
    struct ll_queue* q = conn->instance->router_conns;
    for (uint32_t slot = 0; slot < q->hash_size; slot++) {
        struct ll_entry* entry = q->hash_table[slot];
        while (entry) {
            struct ll_entry* next = entry->hash_next;
            struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)entry;
            if (rconn->send_waiter_q == conn->send_input_q) router_cancel_send_waiter(rconn);
            if (rconn->recv_conn == conn) {
                DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE,
                            "router: clear recv_conn remote=%016llx svc_id=%u (conn destroyed)",
                            (unsigned long long)rconn->remote_node_id, rconn->svc_id);
                rconn->recv_conn = NULL;
            }
            entry = next;
        }
    }
}

// ====================================================================
// Управление ROUTER_CONN
// ====================================================================

#define ROUTER_CONN_KEY_SIZE (sizeof(uint64_t) + SC_PUBKEY_SIZE + 1)
_Static_assert(offsetof(struct ETCP_ROUTER_CONN, svc_id) + 1 - offsetof(struct ETCP_ROUTER_CONN, group_id) == ROUTER_CONN_KEY_SIZE,
               "router channel index layout");

struct ETCP_ROUTER_CONN* etcp_router_conn_lookup(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc) {
    if (!inst || !inst->router_conns || !ed) return NULL;
    uint8_t key[ROUTER_CONN_KEY_SIZE];
    memcpy(key, &group, sizeof(group)); memcpy(key + sizeof(group), ed, SC_PUBKEY_SIZE); key[sizeof(key) - 1] = svc;
    return (struct ETCP_ROUTER_CONN*)queue_find_data_by_index(inst->router_conns, key);
}

static struct ETCP_ROUTER_CONN* router_conn_find(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, uint8_t svc) {
    struct ETCP_ROUTER_CONN* found = NULL;
    for (struct ll_entry* e = inst && inst->router_conns ? inst->router_conns->head : NULL; e; e = e->next) {
        struct ETCP_ROUTER_CONN* c = (struct ETCP_ROUTER_CONN*)e;
        if (c->group_id != group || c->remote_node_id != peer || c->svc_id != svc) continue;
        if (found) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router ambiguous channel group=%016llx peer=%016llx svc=%u",
                (unsigned long long)group, (unsigned long long)peer, svc); return NULL;
        }
        found = c;
    }
    return found;
}

// ====================================================================
// Отправка: build+encode при дренировании send_q, дренирование через waiter на send_input_q
// ====================================================================

// Найти ETCP_CONN для отправки (учитывая indirect-посредников). NULL = нет маршрута.
static struct ETCP_CONN* router_route_conn(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t remote_node_id, uint8_t svc_id, const uint8_t* ed) {
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    struct ETCP_CONN* conn;
    if (group) {
        struct TOPO_GROUP_NODE* unique = ed ? NULL : topo_node_find_by_id(group, remote_node_id);
        const uint8_t* target_ed = ed ? ed : unique ? unique->ed25519 : NULL;
        conn = conn_mgr_route_conn(group->conn_mgr, target_ed, svc_id == ETCP_RT_ID_CONN_MGR);
        if (!conn) conn = ed ? router_group_next(group, ed) : topo_group_find_conn_for_node(group, remote_node_id);
    } else {
        /* Без группы — глобальная маршрутизация: прямое соединение по node_id. */
        if (ed) {
            uint8_t x[32]; if (sc_identity_public_x("direct route", ed, x) != SC_OK) return NULL;
            conn = instance_find_conn_by_key(inst, x);
        } else conn = instance_find_conn(inst, remote_node_id);
    }
    if (inst->router_bindings.tx_policy[svc_id] && conn &&
        (!conn->links_up || conn->close_requested || conn->state == 2 || !conn->send_input_q)) return NULL;
    return conn;
}

// Тонкая обёртка над router_route_conn для rconn (next_hop для отправки).
static struct ETCP_CONN* router_send_conn(struct ETCP_ROUTER_CONN* rconn) {
    return router_route_conn(rconn->inst, rconn->group_id, rconn->remote_node_id, rconn->svc_id, rconn->remote_ed);
}

// Есть ли физический маршрут до узла (прямой/indirect/глобальный): 1 — есть, 0 — нет.
// Локальная доставка (dst == self) всегда возвращает 1.
int etcp_router_has_route(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t dst_node_id) {
    if (!inst) return 0;
    if (dst_node_id == inst->node_id) return 1;
    return router_route_conn(inst, group_id, dst_node_id, 0, NULL) != NULL;
}

int etcp_router_peer_reachable(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc) {
    if (!inst || !ed) return 0;
    if (!memcmp(ed, inst->my_ed25519_pubkey, 32)) return 1;
    uint8_t x[32];
    return sc_identity_public_x("router reachability", ed, x) == SC_OK &&
        router_route_conn(inst, group, sc_derive_node_id_from_pubkey(x), svc, ed);
}

int etcp_router_group_peer_ready(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], uint8_t svc) {
    if (!inst || !ed) return 0;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    uint8_t x[32];
    if (!group || sc_identity_public_x("ready router path", ed, x) != SC_OK) return 0;
    struct ETCP_CONN* conn = router_route_conn(inst, group_id, sc_derive_node_id_from_pubkey(x), svc, ed);
    return conn && topo_group_connection_ready(group, conn);
}

static struct ETCP_ROUTER_PATH router_get_path(struct UTUN_INSTANCE* inst, uint64_t group_id,
                                               uint64_t dst_node_id, const uint8_t* ed, uint8_t svc_id) {
    struct ETCP_ROUTER_PATH path = {0};
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "path snapshot without instance"); return path; }
    struct ETCP_CONN* conn = router_route_conn(inst, group_id, dst_node_id, svc_id, ed);
    const uint8_t* target = ed ? ed : conn && conn->peer_node_id == dst_node_id ? conn->peer_ed25519_pubkey : NULL;
    path.type = conn_mgr_connection_type(conn, target);
    if (path.type != CONN_TYPE_NONE) {
        path.next_hop_node_id = conn->peer_node_id;
        memcpy(path.next_hop_ed, conn->peer_ed25519_pubkey, sizeof(path.next_hop_ed));
        path.tx_epoch = conn->reset_id;
        path.wire_tx_bytes = conn->diagnostic_wire_tx_bytes;
    }
    return path;
}

struct ETCP_ROUTER_PATH etcp_router_get_path(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t dst, uint8_t svc) {
    return router_get_path(inst, group_id, dst, NULL, svc);
}

struct ETCP_ROUTER_PATH etcp_router_get_path_key(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], uint8_t svc) {
    uint8_t x[32];
    if (sc_identity_public_x("router path", ed, x) != SC_OK) return (struct ETCP_ROUTER_PATH){0};
    return router_get_path(inst, group_id, sc_derive_node_id_from_pubkey(x), ed, svc);
}

/* Локальные метаданные pending; время enqueue переживает переход в inflight и повторы. */
struct router_pending { uint64_t enqueued_us; uint32_t seq; uint8_t mode; };

// Собрать и закодировать (подпись/шифрование) пакет в начале отправки.
// Возвращает финальный wire-пакет [SVC_ROUTE_HDR][payload][sig?] (u_malloc).
static int router_build_packet(struct ETCP_ROUTER_CONN* rconn, struct ll_entry* pending,
                               uint8_t** out, size_t* out_len) {
    size_t len = SVC_ROUTE_HDR_SIZE + pending->len;
    uint8_t* base = u_calloc(1, len);
    if (!base) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router build: allocation failed"); return -1; }
    struct SVC_ROUTE_HDR* hdr = (struct SVC_ROUTE_HDR*)base;
    hdr->cmd = ETCP_ID_SVC_ROUTE;
    hdr->group_id = rconn->group_id;
    hdr->dst_node_id = rconn->remote_node_id;
    hdr->src_node_id = rconn->inst->node_id;
    memcpy(hdr->src_ed, rconn->inst->my_ed25519_pubkey, 32); memcpy(hdr->dst_ed, rconn->remote_ed, 32);
    hdr->seq = ((struct router_pending*)pending->data)->seq;
    hdr->svc_id = rconn->svc_id;
    hdr->reset_id = rconn->reset_id;
    hdr->peer_reset_id = rconn->peer_reset_id;
    hdr->timestamp = get_current_timestamp();
    if (router_group_address(rconn->inst, hdr)) { u_free(base); return -1; }
    memcpy(base + SVC_ROUTE_HDR_SIZE, pending->dgram, pending->len);
    uint8_t x[32], ed[32]; int mode = ((struct router_pending*)pending->data)->mode;
    if ((mode & ROUTE_CRYPTO_ENCRYPT) && router_peer_keys(rconn->inst, rconn->group_id, rconn->remote_node_id, rconn->remote_ed, x, ed)) {
        u_free(base); return -1;
    }
    int ret = route_crypto_encode(rconn->inst, router_crypto_format(rconn->inst, rconn->group_id),
        mode & ROUTE_CRYPTO_ENCRYPT ? x : NULL, base, len, mode, out, out_len);
    u_free(base);
    return ret;
}

// Управляющие пакеты используют ту же пару идентификаторов, что DATA.
static void router_control(struct ETCP_ROUTER_CONN* rconn, struct ETCP_CONN* conn, uint8_t flags,
                           uint64_t peer_id, uint64_t challenge) {
    if (!conn) conn = router_send_conn(rconn);
    if (!conn) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "router control: no route svc=%u", rconn->svc_id); return;
    }
    struct ll_entry* e = ll_alloc_lldgram(SVC_ROUTE_HDR_SIZE);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router control: allocation failed"); return; }
    struct SVC_ROUTE_HDR* h = (struct SVC_ROUTE_HDR*)e->dgram;
    memset(h, 0, sizeof(*h));
    h->cmd = ETCP_ID_SVC_ROUTE; h->group_id = rconn->group_id;
    h->src_node_id = rconn->inst->node_id; h->dst_node_id = rconn->remote_node_id; h->svc_id = rconn->svc_id;
    memcpy(h->src_ed, rconn->inst->my_ed25519_pubkey, 32); memcpy(h->dst_ed, rconn->remote_ed, 32);
    h->flags = flags; h->reset_id = rconn->reset_id; h->peer_reset_id = peer_id; h->challenge = challenge;
    e->len = SVC_ROUTE_HDR_SIZE;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE,
                "router control: tx group=%016llx peer=%016llx svc=%u flags=%02x local=%016llx remote=%016llx challenge=%016llx",
                (unsigned long long)rconn->group_id, (unsigned long long)rconn->remote_node_id, rconn->svc_id, flags,
                (unsigned long long)h->reset_id, (unsigned long long)peer_id, (unsigned long long)challenge);
    if (router_source_send(conn, e, 0) != 0) { DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router control: send failed"); free_entry(e); }
}

static void router_send_ctrl(struct ETCP_ROUTER_CONN* rconn, uint8_t flags) {
    if (rconn->peer_reset_id) router_control(rconn, NULL, flags, rconn->peer_reset_id, 0);
}

static int router_random_id(uint64_t* id) {
    if (random_bytes((uint8_t*)id, sizeof(*id)) != 0 || !*id) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router: failed to generate nonzero session nonce");
        return -1;
    }
    return 0;
}

static void router_handshake_start(struct ETCP_ROUTER_CONN* rconn) {
    if (rconn->closed || rconn->peer_reset_id || rconn->handshake_timer) return;
    router_control(rconn, NULL, ROUTER_FLAG_START, 0, 0);
    rconn->handshake_timer = uasync_set_timeout(rconn->inst->ua, ROUTER_HANDSHAKE_TIMEOUT_TB,
                                               rconn, router_handshake_timer_cb, "router_handshake");
}

static void router_handshake_timer_cb(void* arg) {
    struct ETCP_ROUTER_CONN* rconn = arg;
    rconn->handshake_timer = NULL;
    if (rconn->closed || rconn->peer_reset_id) return;
    router_handshake_start(rconn);
}

// Незнакомый идентификатор не сбрасывает рабочую сессию: сначала проверяем живость challenge/echo.
static void router_challenge_peer(struct ETCP_ROUTER_CONN* rconn, struct ETCP_CONN* conn, uint64_t peer_id) {
    uint64_t now = get_time_tb();
    if (rconn->pending_peer_id && now - rconn->challenge_sent_tb < ROUTER_HANDSHAKE_TIMEOUT_TB) return;
    int pending = rconn->pending_peer_id == peer_id && now - rconn->challenge_started_tb < ROUTER_CHALLENGE_LIFETIME_TB;
    if (!pending) {
        if (router_random_id(&rconn->pending_challenge) != 0) return;
        rconn->pending_peer_id = peer_id; rconn->challenge_started_tb = now;
    }
    rconn->challenge_sent_tb = now;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "router challenge: peer=%016llx svc=%u candidate=%016llx nonce=%016llx "
        "repeat=%d age_ms=%llu lifetime_ms=%u",
        (unsigned long long)rconn->remote_node_id, rconn->svc_id, (unsigned long long)peer_id,
        (unsigned long long)rconn->pending_challenge, pending,
        (unsigned long long)((now - rconn->challenge_started_tb) / 10), ROUTER_CHALLENGE_LIFETIME_TB / 10);
    router_control(rconn, conn, ROUTER_FLAG_RST, peer_id, rconn->pending_challenge);
}

// Только ответ на наш свежий challenge разрешает сменить пару эпох. Свой id сохраняем:
// иначе два конца бесконечно перезапускали бы друг друга.
static void router_confirm_peer(struct ETCP_ROUTER_CONN* rconn, const struct SVC_ROUTE_HDR* h) {
    uint64_t age = get_time_tb() - rconn->challenge_started_tb;
    if (h->peer_reset_id != rconn->reset_id || !h->challenge || h->challenge != rconn->pending_challenge
        || h->reset_id != rconn->pending_peer_id || age >= ROUTER_CHALLENGE_LIFETIME_TB) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "router confirm: stale response peer=%016llx svc=%u "
            "received=%016llx/%016llx/%016llx expected=%016llx/%016llx/%016llx age_ms=%llu expired=%d",
            (unsigned long long)rconn->remote_node_id, rconn->svc_id,
            (unsigned long long)h->reset_id, (unsigned long long)h->peer_reset_id, (unsigned long long)h->challenge,
            (unsigned long long)rconn->pending_peer_id, (unsigned long long)rconn->reset_id,
            (unsigned long long)rconn->pending_challenge, (unsigned long long)(age / 10), age >= ROUTER_CHALLENGE_LIFETIME_TB);
        return;
    }
    uint64_t previous = rconn->peer_reset_id;
    uint64_t peer_id = h->reset_id;
    rconn->pending_peer_id = 0; rconn->pending_challenge = 0; rconn->challenge_started_tb = rconn->challenge_sent_tb = 0;
    if (previous == peer_id) return;
    if (previous && router_conn_reset(rconn) != 0) { router_close_and_notify(rconn); return; }
    if (rconn->handshake_timer) { uasync_cancel_timeout(rconn->inst->ua, rconn->handshake_timer); rconn->handshake_timer = NULL; }
    rconn->peer_reset_id = peer_id;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE,
               "router session: group=%016llx peer=%016llx svc=%u local=%016llx remote=%016llx previous=%016llx",
               (unsigned long long)rconn->group_id, (unsigned long long)rconn->remote_node_id, rconn->svc_id,
               (unsigned long long)rconn->reset_id, (unsigned long long)peer_id, (unsigned long long)previous);
    if (previous) router_send_close_to_service(rconn);
    if (!rconn->closed) router_send_kick(rconn);
}

// Доставить локальному сервису событие закрытия conn (пустая кодограмма ROUTER_SVC_HDR_SIZE).
static void router_send_close_to_service(struct ETCP_ROUTER_CONN* rconn) {
    struct UTUN_INSTANCE* inst = rconn->inst;
    const struct router_tx_policy* policy = inst->router_bindings.tx_policy[rconn->svc_id];
    if (policy && policy->closed) policy->closed(inst, rconn->group_id, rconn->remote_ed);
    etcp_recv_fn cb = inst->router_bindings.callbacks[rconn->svc_id];
    if (!cb) return;
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router close notification allocation failed svc=%u", rconn->svc_id); return; }
    e->dgram = u_malloc(ROUTER_SVC_HDR_SIZE);
    if (!e->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router close notification payload allocation failed svc=%u", rconn->svc_id);
        queue_entry_free(e); return;
    }
    e->dgram[0] = rconn->svc_id;
    memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, rconn->remote_ed, 32); memcpy(e->dgram + ROUTER_SVC_DST_ED_OFF, inst->my_ed25519_pubkey, 32);
    e->dgram[ROUTER_SVC_FLAGS_OFF] = 0;
    memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &rconn->group_id, 8);
    e->len = ROUTER_SVC_HDR_SIZE;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router_close_svc: svc_id=%u remote=%016llx",
               rconn->svc_id, (unsigned long long)rconn->remote_node_id);
    struct ETCP_CONN local = {.instance = inst, .peer_node_id = inst->node_id};
    cb(&local, e);
}

// Финальный шаг закрытия: освобождает ll entry (вызывается через uasync_call_soon, уже вне очередей).
static void router_close_finalize(void* arg) {
    struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)arg;
    queue_entry_free(&rconn->ll);
}

static void router_send_queue_free(void* arg) { queue_free(arg); }

// Отменить таймеры/waiter и освободить все очереди rconn (общий код для close/restart).
static void router_conn_free_queues(struct ETCP_ROUTER_CONN* rconn) {
    struct UTUN_INSTANCE* inst = rconn->inst;
    if (rconn->send_request) etcp_router_cancel_request(rconn->send_request);
    if (rconn->receive_pump) { uasync_call_soon_cancel(inst->ua, rconn->receive_pump); rconn->receive_pump = NULL; }
    if (rconn->handshake_timer) { uasync_cancel_timeout(inst->ua, rconn->handshake_timer); rconn->handshake_timer = NULL; }
    if (rconn->ack_timer)      { uasync_cancel_timeout(inst->ua, rconn->ack_timer); rconn->ack_timer = NULL; }
    if (rconn->idle_ack_timer) { uasync_cancel_timeout(inst->ua, rconn->idle_ack_timer); rconn->idle_ack_timer = NULL; }
    if (rconn->watchdog_timer) { uasync_cancel_timeout(inst->ua, rconn->watchdog_timer); rconn->watchdog_timer = NULL; }
    if (rconn->no_route_timer) { uasync_cancel_timeout(inst->ua, rconn->no_route_timer); rconn->no_route_timer = NULL; }
    if (rconn->retrans_timer)  { uasync_cancel_timeout(inst->ua, rconn->retrans_timer); rconn->retrans_timer = NULL; }
    router_cancel_send_waiter(rconn);
    struct ll_entry* f;
    if (rconn->send_q) {
        // Освобождение очереди не должно запускать производителей новых данных.
        queue_set_threshold(rconn->send_q, -1, 0);
        while ((f = queue_data_get(rconn->send_q)) != NULL) { queue_dgram_free(f); queue_entry_free(f); }
        // Producer callback может закрыть/перезапустить rconn внутри queue_data_get(send_q).
        uasync_call_soon(inst->ua, rconn->send_q, router_send_queue_free); rconn->send_q = NULL;
    }
    if (rconn->recv_q) {
        while ((f = queue_data_get(rconn->recv_q)) != NULL) { queue_dgram_free(f); queue_entry_free(f); }
        queue_free(rconn->recv_q); rconn->recv_q = NULL;
    }
    if (rconn->inflight_q) {
        while ((f = queue_data_get(rconn->inflight_q)) != NULL) {
            struct ROUTER_INFLIGHT* inf = (struct ROUTER_INFLIGHT*)f;
            if (inf->dgram) u_free(inf->dgram);
            u_free(inf);
        }
        queue_free(rconn->inflight_q); rconn->inflight_q = NULL;
    }
    if (rconn->incoming_q) {
        while ((f = queue_data_get(rconn->incoming_q)) != NULL) { queue_dgram_free(f); queue_entry_free(f); }
        queue_resume_callback(rconn->incoming_q);
        queue_free(rconn->incoming_q); rconn->incoming_q = NULL;
    }
}

// Пересоздать очереди и сбросить всё состояние rconn (кроме identity: group/remote/svc/ll.data/inst/reset_id/last_dgram_ts).
static int router_conn_reset(struct ETCP_ROUTER_CONN* rconn) {
    router_conn_free_queues(rconn);

    rconn->send_q     = queue_new(rconn->inst->ua, 0, 0, 0, "router_send_q");
    rconn->recv_q     = queue_new(rconn->inst->ua, ROUTER_RECVQ_HASH_SIZE, 0, 4, "router_recv_q");
    rconn->inflight_q = queue_new(rconn->inst->ua, ROUTER_INFLIGHT_HASH_SIZE, 0, 4, "router_inflight_q");
    rconn->incoming_q = queue_new(rconn->inst->ua, 0, 0, 0, "router_incoming_q");
    if (!rconn->send_q || !rconn->recv_q || !rconn->inflight_q || !rconn->incoming_q) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router reset: queue allocation failed");
        router_conn_free_queues(rconn); return -1;
    }
    queue_set_threshold(rconn->send_q, ROUTER_MAX_SEND_Q_PACKETS - 1, 0);
    queue_set_callback(rconn->incoming_q, router_incoming_q_cb, rconn);
    queue_set_threshold(rconn->incoming_q, 0, 0);
    rconn->receive_blocked = 0;
    rconn->peer_receive_blocked = 0;
    rconn->recv_conn = NULL;

    rconn->rtt = 0; rconn->rtt_jitter = 0; rconn->delivery_rtt_us = 0;
    rconn->max_inflight   = ROUTER_MAX_INFLIGHT;
    rconn->inflight_limit = ROUTER_MAX_INFLIGHT;
    rconn->minrtt = MINRTT_DEFAULT_TB;
    memset(rconn->minrtt_window, 0, sizeof(rconn->minrtt_window));
    memset(rconn->minrtt_window_tb, 0, sizeof(rconn->minrtt_window_tb));
    rconn->minrtt_window_idx = 0; rconn->minrtt_window_count = 0; rconn->minrtt_probe = 0;
    rconn->last_inflight_loaded_tb = 0; rconn->last_inflight_unloaded_tb = 0; rconn->inflight_was_loaded = 0;
    rconn->last_recv_pkt_ts = 0; rconn->last_recv_pkt_local_tb = 0; rconn->last_recv_updated = 0;

    rconn->tx_seq = 0; rconn->tx_sent = 0; rconn->rx_seq = 0; rconn->tx_acked = 0;
    rconn->retrans_seq = 0; rconn->retrans_end = 0;
    rconn->last_sent_ack_seq = 0; rconn->last_ack_sent_tb = 0;
    rconn->send_blocked = 0; rconn->start_sent = 0;
    rconn->peer_reset_id = 0;
    rconn->no_route = 0; rconn->no_route_timer = NULL; rconn->no_ack_count = 0; rconn->closed = 0;
    rconn->c_pkts_sent = 0; rconn->c_pkts_send_err = 0; rconn->c_pkts_rcvd = 0;
    rconn->c_ack_sent = 0; rconn->c_ack_recv = 0; rconn->c_retrans_done = 0;
    rconn->c_dup_dropped = 0; rconn->c_oob_dropped = 0; rconn->c_stale_ack = 0; rconn->c_sign_fail = 0;
    rconn->last_ack_changed_tb = 0;
    rconn->retrans_due_tb = 0; rconn->retrans_srtt_tb = 0; rconn->retrans_rttvar_tb = 0; rconn->retrans_started = 0;
    route_tx_meter_reset(&rconn->delivery, get_time_us());
    return 0;
}

// Снять канал с реестра, уведомить сервис/пира, очистить очереди/таймеры; само ll освободить через call_soon.
static void router_close_and_notify(struct ETCP_ROUTER_CONN* rconn) {
    if (!rconn || rconn->closed) return;
    struct UTUN_INSTANCE* inst = rconn->inst;
    rconn->closed = 1;
    // Callback может закрыть группу или открыть новый канал с тем же ключом.
    if (inst->router_conns) queue_remove_data(inst->router_conns, &rconn->ll);
    router_send_close_to_service(rconn);
    // Отправляем CLOSE удалённой стороне + снимаем waiter отправки
    if (router_send_conn(rconn)) router_send_ctrl(rconn, ROUTER_FLAG_CLOSE);
    router_conn_free_queues(rconn);
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router_conn: closed group=%016llx remote=%016llx svc_id=%u",
               (unsigned long long)rconn->group_id, (unsigned long long)rconn->remote_node_id, rconn->svc_id);
    uasync_call_soon(inst->ua, rconn, router_close_finalize);
}

// Пометить отсутствие маршрута и взвести таймер повторных проверок (идемпотентно).
static void router_set_no_route(struct ETCP_ROUTER_CONN* rconn) {
    if (rconn->no_route) return;
    rconn->no_route = 1;
    DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router: no route to %016llx svc_id=%u group=%016llx — send_q=%d queued, will retry",
               (unsigned long long)rconn->remote_node_id, rconn->svc_id,
               (unsigned long long)rconn->group_id, queue_entry_count(rconn->send_q));
    if (!rconn->no_route_timer)
        rconn->no_route_timer = uasync_set_timeout(rconn->inst->ua,
            ROUTER_NO_ROUTE_RETRY_TB, rconn, router_no_route_retry_cb, "router_no_route");
}

// Попытаться продвинуть отправку: зарегистрировать waiter на send_input_q, если есть что слать.
// Инвариант: send_q непуст ∧ !closed ∧ !no_route ∧ inflight свободен ⇒ waiter зарегистрирован.
static void router_send_kick(struct ETCP_ROUTER_CONN* rconn) {
    if (rconn->closed || rconn->no_route || rconn->send_producing) return;
    if (!rconn->peer_reset_id) {
        if (queue_entry_count(rconn->send_q) || rconn->send_request) {
            router_handshake_start(rconn);
        }
        return;
    }
    int retry = rconn->retrans_seq != rconn->retrans_end;
    if (rconn->peer_receive_blocked && !retry) return;
    if (!retry && queue_entry_count(rconn->send_q) == 0 && !rconn->send_request) return;
    if (!retry && queue_entry_count(rconn->inflight_q) >= (int)router_effective_max_inflight(rconn)) {
        rconn->send_blocked = 1;
        return;
    }
    struct ETCP_CONN* conn = router_send_conn(rconn);
    if (!conn || !conn->send_input_q || conn->state == 2) { router_cancel_send_waiter(rconn); router_set_no_route(rconn); return; }
    if (rconn->send_waiter_q != conn->send_input_q) router_cancel_send_waiter(rconn);
    if (rconn->send_waiter.internal || rconn->send_waiter.call_soon_id) return;
    rconn->send_waiter_q = conn->send_input_q;
    if (queue_waiter_wait(conn->send_input_q, &rconn->send_waiter, router_send_drain_cb, rconn) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router send: waiter registration failed svc=%u", rconn->svc_id);
}

// Waiter-коллбэк: send_input_q опустел → кодируем и шлём один пакет, при необходимости
// снова встаём в хвост (round-robin). Успешно отправленный пакет кладём в inflight_q для ретрансмита.
static void router_send_drain_cb(struct ll_queue* q, void* arg) {
    struct ETCP_ROUTER_CONN* rconn = arg;
    rconn->send_waiter_q = NULL;
    if (rconn->closed || rconn->no_route) return;
    if (!router_allowed(rconn->inst, rconn->group_id, rconn->remote_ed, rconn->svc_id)) {
        router_close_and_notify(rconn); return;
    }
    struct ETCP_CONN* conn = router_send_conn(rconn);
    if (!conn || conn->send_input_q != q) { router_send_kick(rconn); return; }
    int retry_pending = rconn->retrans_seq != rconn->retrans_end;
    while (rconn->retrans_seq != rconn->retrans_end) {
        uint32_t seq = rconn->retrans_seq;
        struct ROUTER_INFLIGHT* inf = (struct ROUTER_INFLIGHT*)queue_find_data_by_index(rconn->inflight_q, &seq);
        if (!inf) { rconn->retrans_seq++; continue; }
        if (get_time_tb() - inf->last_sent_tb < router_retrans_rto(rconn)) { rconn->retrans_seq++; continue; }
        if (router_retransmit_one(rconn, inf) != 0) return;
        rconn->retrans_seq++;
        router_retrans_schedule(rconn);
        router_send_kick(rconn);
        return;
    }
    if (retry_pending) router_retrans_schedule(rconn);
    if (rconn->peer_receive_blocked) return;
    if (queue_entry_count(rconn->inflight_q) >= (int)router_effective_max_inflight(rconn)) {
        rconn->send_blocked = 1;
        return;
    }
    if (!rconn->send_q->head && rconn->send_request) {
        struct router_send_request *h = rconn->send_request;
        void (*ready)(struct ETCP_ROUTER_CONN*, void*) = h->ready;
        void *context = h->arg;
        uint64_t local = rconn->reset_id, peer = rconn->peer_reset_id;
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "router producer ready svc=%u waited_us=%llu inflight=%d/%u",
                    rconn->svc_id, (unsigned long long)(get_time_us() - h->since_us),
                    queue_entry_count(rconn->inflight_q), router_effective_max_inflight(rconn));
        rconn->send_request = NULL; memset(h, 0, sizeof(*h));
        rconn->send_producing = 1;
        ready(rconn, context);
        rconn->send_producing = 0;
        if (rconn->closed) return;
        if (rconn->reset_id != local || rconn->peer_reset_id != peer || router_send_conn(rconn) != conn) {
            router_send_kick(rconn); return;
        }
    }
    struct ll_entry* pending = rconn->send_q->head;
    if (!pending) { rconn->send_blocked = 0; router_send_watchdog_disarm(rconn); return; }
    struct ll_entry* e = queue_entry_new(0);
    struct ROUTER_INFLIGHT* inf = u_calloc(1, sizeof(*inf));
    if (!e || !inf) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router send: allocation failed; packet retained");
        if (e) free_entry(e);
        u_free(inf);
        return;
    }
    size_t len;
    if (router_build_packet(rconn, pending, &inf->dgram, &len) != 0) { free_entry(e); u_free(inf); return; }
    e->dgram = u_malloc(len);
    if (!e->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router send: wire copy failed; packet retained");
        free_entry(e); u_free(inf->dgram); u_free(inf); return;
    }
    memcpy(e->dgram, inf->dgram, len); e->len = len;
    inf->seq = ((struct router_pending*)pending->data)->seq; inf->ll.size = 4; inf->ll.len = pending->len;
    inf->enqueued_us = ((struct router_pending*)pending->data)->enqueued_us;
    inf->dgram_len = len; inf->last_sent_tb = get_time_tb(); inf->send_count = 1;
    uint32_t seq = inf->seq;
    queue_data_put_with_index(rconn->inflight_q, &inf->ll);
    rconn->tx_sent = seq + 1;
    if (router_source_send(conn, e, 0) != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router send failed svc=%u seq=%u; packet retained", rconn->svc_id, seq);
        rconn->tx_sent = seq;
        queue_remove_data(rconn->inflight_q, &inf->ll);
        free_entry(e); u_free(inf->dgram); u_free(inf); rconn->c_pkts_send_err++;
        return;
    }
    uint64_t local_id = rconn->reset_id, peer_id = rconn->peer_reset_id;
    int first_inflight = queue_entry_count(rconn->inflight_q) == 1;
    if (first_inflight) {
        uint64_t now = get_time_tb();
        if (!rconn->inst->router_bindings.tx_policy[rconn->svc_id]) rconn->last_ack_changed_tb = now;
        rconn->no_ack_count = 0;
        rconn->retrans_due_tb = now + router_retrans_rto(rconn);
    }
    free_entry(queue_data_get(rconn->send_q));
    if (rconn->closed || rconn->reset_id != local_id || rconn->peer_reset_id != peer_id) return;
    rconn->c_pkts_sent++;
    rconn->last_dgram_ts = get_current_timestamp();
    if (first_inflight || !rconn->retrans_timer) router_retrans_schedule(rconn);
    router_track_inflight_state(rconn);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "router sent: peer=%016llx svc=%u seq=%u acked=%u inflight=%d pending=%d",
                (unsigned long long)rconn->remote_node_id, rconn->svc_id, seq, rconn->tx_acked,
                queue_entry_count(rconn->inflight_q), queue_entry_count(rconn->send_q));
    router_send_kick(rconn);
}

// DEBUG-watchdog: медленная (500мс) проверка инварианта drain. Если send_q непуст,
// канал свободен, но waiter не зарегистрирован — инвариант нарушен: логируем и форсируем kick.
static void router_send_watchdog_cb(void* arg) {
    struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)arg;
    rconn->watchdog_timer = NULL;
    if (rconn->closed) return;
    if (queue_entry_count(rconn->send_q) == 0 && !rconn->send_request) return;
    // Маршрут мог смениться, пока waiter остаётся на занятой очереди прежнего next hop.
    if (rconn->peer_reset_id && !rconn->no_route) router_send_kick(rconn);
    if ((queue_entry_count(rconn->send_q) > 0 || rconn->send_request) && !rconn->watchdog_timer)
        rconn->watchdog_timer = uasync_set_timeout(rconn->inst->ua,
            ROUTER_SEND_WATCHDOG_TB, rconn, router_send_watchdog_cb, "router_send_watchdog");
}

// Взвести watchdog пока send_q непуст (страховка от пропущенного re-arm).
static void router_send_watchdog_arm(struct ETCP_ROUTER_CONN* rconn) {
    if (rconn->watchdog_timer || (queue_entry_count(rconn->send_q) == 0 && !rconn->send_request)) return;
    rconn->watchdog_timer = uasync_set_timeout(rconn->inst->ua,
        ROUTER_SEND_WATCHDOG_TB, rconn, router_send_watchdog_cb, "router_send_watchdog");
}

// Снять watchdog когда send_q опустел.
static void router_send_watchdog_disarm(struct ETCP_ROUTER_CONN* rconn) {
    if (rconn->watchdog_timer) { uasync_cancel_timeout(rconn->inst->ua, rconn->watchdog_timer); rconn->watchdog_timer = NULL; }
}

// Таймер повторной проверки маршрута (20ms): при появлении — снять no_route и возобновить отправку.
static void router_no_route_retry_cb(void* arg) {
    struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)arg;
    if (rconn->closed) return;
    rconn->no_route_timer = NULL;
    if (router_send_conn(rconn)) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router: route appeared for %016llx svc_id=%u, resuming send_q=%d",
                   (unsigned long long)rconn->remote_node_id, rconn->svc_id, queue_entry_count(rconn->send_q));
        rconn->no_route = 0;
        router_send_kick(rconn);
        return;
    }
    rconn->no_route_timer = uasync_set_timeout(rconn->inst->ua,
        ROUTER_NO_ROUTE_RETRY_TB, rconn, router_no_route_retry_cb, "router_no_route");
}

// ====================================================================
// Ретрансмиты
// ====================================================================

// Отправить повторно один inflight-пакет (финальный wire-пакет уже закодирован — шлём копию как есть).
static int router_retransmit_one(struct ETCP_ROUTER_CONN* rconn, struct ROUTER_INFLIGHT* inf) {
    struct ETCP_CONN* conn = router_send_conn(rconn);
    if (!conn) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router_retransmit: no route to %016llx svc_id=%u",
                   (unsigned long long)rconn->remote_node_id, rconn->svc_id);
        rconn->c_pkts_send_err++; return -1;
    }
    // Финальный пакет уже закодирован (подпись/шифрование) — шлём как есть.
    struct ll_entry* entry = queue_entry_new(0);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router retransmit: allocation failed"); rconn->c_pkts_send_err++; return -1; }
    entry->dgram = u_malloc(inf->dgram_len);
    if (!entry->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router retransmit: payload allocation failed"); queue_entry_free(entry); rconn->c_pkts_send_err++; return -1; }
    memcpy(entry->dgram, inf->dgram, inf->dgram_len);
    entry->len = inf->dgram_len;

    rconn->last_dgram_ts = get_current_timestamp();
    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "RETRANS: svc_id=%u seq=%u attempt=%d → %016llx",
               rconn->svc_id, inf->seq, inf->send_count + 1, (unsigned long long)rconn->remote_node_id);
    int ret = router_source_send(conn, entry, 1);
    if (ret != 0) {
        queue_dgram_free(entry); queue_entry_free(entry);
        rconn->c_pkts_send_err++;
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router retransmit: send failed seq=%u", inf->seq);
    } else {
        inf->last_sent_tb = get_time_tb();
        if (inf->send_count < UINT8_MAX) inf->send_count++;
        if (!rconn->retrans_started) {
            rconn->retrans_started = 1;
            if (rconn->no_ack_count < UINT8_MAX) rconn->no_ack_count++;
        }
        rconn->retrans_due_tb = inf->last_sent_tb + router_retrans_interval(rconn);
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE,
                    "router retry sent: peer=%016llx svc=%u seq=%u attempt=%u round=%u next_ms=%llu stalled_ms=%llu",
                    (unsigned long long)rconn->remote_node_id, rconn->svc_id, inf->seq, inf->send_count, rconn->no_ack_count,
                    (unsigned long long)(router_retrans_interval(rconn) / 10),
                    (unsigned long long)((inf->last_sent_tb - rconn->last_ack_changed_tb) / 10));
        rconn->c_retrans_done++;
        rconn->c_pkts_sent++;
    }
    return ret;
}

// Базовый RTO учитывает нижние очереди и разброс доставки, но не собственную send_q.
static uint64_t router_retrans_rto(const struct ETCP_ROUTER_CONN* rconn) {
    uint64_t rto = rconn->retrans_srtt_tb + 4 * rconn->retrans_rttvar_tb;
    return rto > ROUTER_RETRANS_MIN_TB ? rto : ROUTER_RETRANS_MIN_TB;
}

// Backoff растёт только после фактического повтора, с насыщением без переполнения сдвига.
static uint64_t router_retrans_interval(const struct ETCP_ROUTER_CONN* rconn) {
    uint64_t interval = router_retrans_rto(rconn);
    uint64_t limit = interval > ROUTER_RETRANS_BACKOFF_TB ? interval : ROUTER_RETRANS_BACKOFF_TB;
    for (unsigned i = 0; i < rconn->no_ack_count && interval < limit; i++)
        interval = interval > limit / 2 ? limit : interval * 2;
    return interval;
}

// Срок закрытия не зависит от числа повторов и не продлевается нашими отправками.
static uint64_t router_no_ack_timeout(const struct ETCP_ROUTER_CONN* rconn) {
    const struct router_tx_policy* policy = rconn->inst->router_bindings.tx_policy[rconn->svc_id];
    if (policy) return policy->ack_timeout_tb;
    int call = rconn->svc_id == ETCP_RT_ID_CALL || rconn->svc_id == ETCP_RT_ID_CALL_AUDIO
            || rconn->svc_id == ETCP_RT_ID_CALL_VIDEO || rconn->svc_id == ETCP_RT_ID_CALL_VIDEO_CONTROL;
    uint64_t timeout = call ? ROUTER_CALL_NO_ACK_TIMEOUT_TB : ROUTER_NO_ACK_TIMEOUT_TB;
    uint64_t adaptive = 4 * router_retrans_rto(rconn);
    return timeout > adaptive ? timeout : adaptive;
}

// Перевзвести на ближайший срок повтора/закрытия. Пока проход ждёт waiter, новый не назначается.
static void router_retrans_schedule(struct ETCP_ROUTER_CONN* rconn) {
    if (rconn->retrans_timer) {
        uasync_cancel_timeout(rconn->inst->ua, rconn->retrans_timer); rconn->retrans_timer = NULL;
    }
    if (rconn->closed) return;
    int inflight = queue_entry_count(rconn->inflight_q);
    if (!inflight && (!rconn->inst->router_bindings.tx_policy[rconn->svc_id] || !queue_entry_count(rconn->send_q))) return;
    uint64_t now = get_time_tb();
    uint64_t due = rconn->last_ack_changed_tb + router_no_ack_timeout(rconn);
    if (inflight && rconn->retrans_seq == rconn->retrans_end && rconn->retrans_due_tb < due) due = rconn->retrans_due_tb;
    uint64_t remaining = due > now ? due - now : 1;
    if (remaining > UINT32_MAX) remaining = UINT32_MAX;
    rconn->retrans_timer = uasync_set_timeout(rconn->inst->ua,
        (uint32_t)remaining, rconn, router_retrans_timer_cb, "router_retrans");
    if (!rconn->retrans_timer) DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router retransmit: timer allocation failed svc=%u", rconn->svc_id);
}

// Редкое восстановление окна при застое ACK. Занятость приёмника и ожидание очереди не являются попытками.
static void router_retrans_timer_cb(void* arg) {
    struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)arg;
    if (rconn->closed) return;
    rconn->retrans_timer = NULL;
    int inflight = queue_entry_count(rconn->inflight_q);
    int queued = queue_entry_count(rconn->send_q);
    if (!inflight && (!queued || !rconn->inst->router_bindings.tx_policy[rconn->svc_id])) return;
    uint64_t now = get_time_tb();
    uint64_t elapsed = now - rconn->last_ack_changed_tb;

    if (elapsed >= router_no_ack_timeout(rconn)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE,
                   "router ACK timeout: peer=%016llx svc=%u stalled_ms=%llu rto_ms=%llu rounds=%u inflight=%d queued=%d busy=%u no_route=%u",
                   (unsigned long long)rconn->remote_node_id, rconn->svc_id, (unsigned long long)(elapsed / 10),
                   (unsigned long long)(router_retrans_rto(rconn) / 10), rconn->no_ack_count,
                   inflight, queued, rconn->peer_receive_blocked, rconn->no_route);
        router_close_and_notify(rconn);
        return;
    }
    if (inflight && now >= rconn->retrans_due_tb && rconn->retrans_seq == rconn->retrans_end) {
        rconn->retrans_due_tb = now + router_retrans_interval(rconn);
        if (!rconn->peer_receive_blocked || elapsed >= router_retrans_rto(rconn)) {
            rconn->retrans_seq = rconn->tx_acked;
            // Если BUSY перестал обновляться, один дубликат запрашивает актуальное состояние/потерянный ACK.
            rconn->retrans_end = rconn->peer_receive_blocked ? rconn->tx_acked + 1 : rconn->tx_sent;
            rconn->retrans_started = 0;
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE,
                        "router retry scheduled: peer=%016llx svc=%u range=[%u,%u) rto_ms=%llu stalled_ms=%llu queued=%d busy_probe=%u",
                        (unsigned long long)rconn->remote_node_id, rconn->svc_id,
                        rconn->retrans_seq, rconn->retrans_end, (unsigned long long)(router_retrans_rto(rconn) / 10),
                        (unsigned long long)(elapsed / 10), queue_entry_count(rconn->send_q), rconn->peer_receive_blocked);
            router_send_kick(rconn);
        }
    }
    router_retrans_schedule(rconn);
}

size_t etcp_router_payload_limit(int mode) {
    size_t overhead = (mode & ROUTE_CRYPTO_SIGN) ? SC_SIGN_SIZE : 0;
    if (mode & ROUTE_CRYPTO_ENCRYPT) overhead += ROUTER_ENCRYPTION_OVERHEAD;
    return ROUTER_MAX_ENCODED_BODY - overhead;
}

// Скопировать plaintext и mode в send_q, выделить seq; кодирование выполняется при отправке после handshake.
static int router_enqueue_send(struct ETCP_ROUTER_CONN* rconn, const uint8_t* payload, size_t len, int force, int mode) {
    if (!router_allowed(rconn->inst, rconn->group_id, rconn->remote_ed, rconn->svc_id)) return -1;
    if (mode & ~(ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router send: invalid crypto mode=%x", mode); return -1;
    }
    if (router_crypto_format(rconn->inst, rconn->group_id) == ROUTE_CRYPTO_CHAT) mode = ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT;
    uint8_t required = router_policy_applies(rconn->inst, rconn->group_id, rconn->svc_id)
        ? rconn->inst->router_bindings.required_crypto[rconn->svc_id] : 0;
    if ((mode & required) != required) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router send: insufficient crypto svc=%u peer=%016llx flags=%02x required=%02x",
                    rconn->svc_id, (unsigned long long)rconn->remote_node_id, mode, required);
        return -1;
    }
    size_t limit = etcp_router_payload_limit(mode);
    if (rconn->closed || !payload || !len || len > limit) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router send: invalid payload len=%zu max=%zu mode=%x closed=%u", len, limit, mode, rconn->closed);
        return -1;
    }
    if (!force && queue_entry_count(rconn->send_q) >= ROUTER_MAX_SEND_Q_PACKETS) return -1;
    struct ll_entry* e = queue_entry_new(sizeof(struct router_pending));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router enqueue: allocation failed"); return -1; }
    e->dgram = u_malloc(len);
    if (!e->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router enqueue: payload allocation failed"); queue_entry_free(e); return -1; }
    *(struct router_pending*)e->data = (struct router_pending){get_time_us(), rconn->tx_seq, (uint8_t)mode};
    memcpy(e->dgram, payload, len); e->len = len;
    DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, "route enqueue peer=%016llx group=%016llx epoch=%016llx svc=%u seq=%u bytes=%zu "
                "queued=%d inflight=%d limit=%u ready=%u no_route=%u peer_busy=%u waiter=%u",
                (unsigned long long)rconn->remote_node_id, (unsigned long long)rconn->group_id,
                (unsigned long long)rconn->reset_id, rconn->svc_id, rconn->tx_seq, len,
                queue_entry_count(rconn->send_q), queue_entry_count(rconn->inflight_q), rconn->inflight_limit,
                rconn->peer_reset_id != 0, rconn->no_route, rconn->peer_receive_blocked, rconn->send_waiter_q != NULL);
    int start_ack_wait = rconn->inst->router_bindings.tx_policy[rconn->svc_id] &&
        !queue_entry_count(rconn->send_q) && !queue_entry_count(rconn->inflight_q);
    rconn->tx_seq++;
    queue_data_put(rconn->send_q, e);
    if (start_ack_wait) {
        rconn->last_ack_changed_tb = get_time_tb();
        router_retrans_schedule(rconn);
    }
    router_send_kick(rconn);
    router_send_watchdog_arm(rconn);
    return 0;
}

// ====================================================================
// minRTT helpers
// ====================================================================

// Эффективный лимит inflight для текущего режима (probe или обычный).
static uint32_t router_effective_max_inflight(struct ETCP_ROUTER_CONN* rconn) {
    return rconn->inflight_limit;
}

// Пересчитать inflight_limit (=MINRTT_PROBE_MAX_INFLIGHT в probe-режиме, иначе max_inflight) и
// скорректировать send_blocked / возобновить отправку при росте лимита.
static void router_update_inflight_limit(struct ETCP_ROUTER_CONN* rconn) {
    uint32_t new_limit = rconn->minrtt_probe ? MINRTT_PROBE_MAX_INFLIGHT : rconn->max_inflight;
    if (new_limit == rconn->inflight_limit) return;
    uint32_t old = rconn->inflight_limit;
    rconn->inflight_limit = new_limit;
    uint32_t inflight = (uint32_t)queue_entry_count(rconn->inflight_q);

    if (new_limit < old && inflight >= new_limit) {
        if (!rconn->send_blocked) rconn->send_blocked = 1;
    }
    if (new_limit > old && (rconn->send_blocked || queue_entry_count(rconn->send_q) > 0 || rconn->send_request)) {
        rconn->send_blocked = 0;
        router_send_kick(rconn);
    }
}

// Установить рабочий max_inflight (не ниже MINRTT_PROBE_MAX_INFLIGHT), пересчитать лимит и состояние канала.
void etcp_router_set_max_inflight(struct ETCP_ROUTER_CONN* rconn, uint16_t new_max) {
    if (!rconn) return;
    if (new_max < MINRTT_PROBE_MAX_INFLIGHT) new_max = MINRTT_PROBE_MAX_INFLIGHT;
    rconn->max_inflight = new_max;
    if (!rconn->minrtt_probe) router_update_inflight_limit(rconn);
    router_track_inflight_state(rconn);
}

// Отслеживать состояние загрузки канала: фиксирует моменты перехода loaded/unloaded
// (inflight >= max_inflight/2) для последующей оценки minRTT.
static void router_track_inflight_state(struct ETCP_ROUTER_CONN* rconn) {
    uint32_t inflight = (uint32_t)queue_entry_count(rconn->inflight_q);
    uint32_t threshold = rconn->max_inflight / 2;
    uint8_t is_loaded = (inflight >= threshold);
    if (is_loaded && !rconn->inflight_was_loaded) {
        rconn->last_inflight_loaded_tb = get_time_tb();
        rconn->inflight_was_loaded = 1;
    } else if (!is_loaded && rconn->inflight_was_loaded) {
        rconn->last_inflight_unloaded_tb = get_time_tb();
        rconn->inflight_was_loaded = 0;
    }
}

// Обновить minRTT: в probe-режиме (старые замеры > MINRTT_PROBE_TIMEOUT_TB) ограничить inflight,
// при разгруженном канале достаточно долго — добавить свежий замер rtt в окно и пересчитать среднее.
static void router_update_minrtt(struct ETCP_ROUTER_CONN* rconn) {
    uint64_t now = get_time_tb();
    uint16_t w_idx = rconn->minrtt_window_idx;
    uint8_t  w_cnt = rconn->minrtt_window_count;

    // Проверка probe: свежесть самого старого замера в окне
    uint8_t prev_probe = rconn->minrtt_probe;
    if (w_cnt > 0) {
        uint8_t oldest = (w_idx - w_cnt + MINRTT_WINDOW_SIZE) % MINRTT_WINDOW_SIZE;
        uint64_t oldest_age = now - rconn->minrtt_window_tb[oldest];
        if (w_cnt >= MINRTT_WINDOW_SIZE && oldest_age < MINRTT_PROBE_TIMEOUT_TB) {
            rconn->minrtt_probe = 0;
        } else if (oldest_age >= MINRTT_PROBE_TIMEOUT_TB) {
            rconn->minrtt_probe = 1;
        }
    }
    if (prev_probe != rconn->minrtt_probe) router_update_inflight_limit(rconn);

    // Обновить minRTT, если канал разгружен достаточно долго
    if (!rconn->inflight_was_loaded) {
        uint64_t unloaded_dur = now - rconn->last_inflight_unloaded_tb;
        uint64_t min_dur = (uint64_t)rconn->rtt * 2;
        if (min_dur < MINRTT_UNLOADED_MIN_TB) min_dur = MINRTT_UNLOADED_MIN_TB;
        if (unloaded_dur >= min_dur) {
            rconn->minrtt_window[w_idx] = rconn->rtt;
            rconn->minrtt_window_tb[w_idx] = now;
            rconn->minrtt_window_idx = (w_idx + 1) % MINRTT_WINDOW_SIZE;
            if (w_cnt < MINRTT_WINDOW_SIZE) w_cnt++;
            rconn->minrtt_window_count = w_cnt;
            uint32_t sum = 0, cnt = 0;
            for (int i = 0; i < MINRTT_WINDOW_SIZE; i++) {
                if (rconn->minrtt_window[i] > 0) { sum += rconn->minrtt_window[i]; cnt++; }
            }
            rconn->minrtt = cnt > 0 ? (uint16_t)(sum / cnt) : MINRTT_DEFAULT_TB;
            DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "MINRTT_UPD: svc_id=%u minrtt=%u (rtt=%u cnt=%u probe=%d unloaded=%lu)",
                        rconn->svc_id, rconn->minrtt, rconn->rtt, cnt, rconn->minrtt_probe, (unsigned long)unloaded_dur);
        }
    }
}

// ====================================================================
// ACK timers
// ====================================================================

// Тонкая обёртка для единообразного вызова отправки ACK из таймеров.
static void router_ack_do_send(struct ETCP_ROUTER_CONN* rconn) {
    router_send_ack(rconn);
}

// Отправить ACK с текущим rx_seq (header-only пакет, seq=rx_seq, payload пуст).
// timestamp — эхо последнего data-пакета пира с поправкой на локальное время (для RTT).
static void router_send_ack(struct ETCP_ROUTER_CONN* rconn) {
    struct UTUN_INSTANCE* inst = rconn->inst;
    struct ETCP_CONN* conn = rconn->recv_conn;
    if (!conn) conn = router_send_conn(rconn);
    if (!conn) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router_ack: no route to %016llx svc_id=%u",
                   (unsigned long long)rconn->remote_node_id, rconn->svc_id);
        rconn->last_ack_sent_tb = get_time_tb();
        return;
    }
    struct SVC_ROUTE_HDR* hdr = (struct SVC_ROUTE_HDR*)u_malloc(SVC_ROUTE_HDR_SIZE);
    if (!hdr) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router_ack: u_malloc failed"); return; }
    hdr->cmd         = ETCP_ID_SVC_ROUTE;
    hdr->group_id    = rconn->group_id;
    hdr->dst_node_id = rconn->remote_node_id;
    hdr->src_node_id = inst->node_id;
    memcpy(hdr->src_ed, inst->my_ed25519_pubkey, 32); memcpy(hdr->dst_ed, rconn->remote_ed, 32);
    hdr->seq         = rconn->rx_seq;
    hdr->svc_id      = rconn->svc_id;
    hdr->flags       = rconn->receive_blocked ? ROUTER_FLAG_RECEIVE_BUSY : 0;
    hdr->reset_id    = rconn->reset_id;
    hdr->peer_reset_id = rconn->peer_reset_id;
    hdr->challenge = 0;
    if (rconn->last_recv_updated) {
        uint64_t now = get_time_tb();
        hdr->timestamp = rconn->last_recv_pkt_ts + (uint32_t)(now - rconn->last_recv_pkt_local_tb);
        rconn->last_recv_updated = 0;
    } else {
        hdr->timestamp = 0;
    }

    struct ll_entry* entry = queue_entry_new(0);
    if (!entry) { u_free(hdr); return; }
    entry->dgram = (uint8_t*)hdr;
    entry->len   = SVC_ROUTE_HDR_SIZE;

    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "ACK_SEND: svc_id=%u rx_seq=%u → %016llx",
                rconn->svc_id, rconn->rx_seq, (unsigned long long)rconn->remote_node_id);
    int ret = router_source_send(conn, entry, 0);
    if (ret != 0) {
        queue_dgram_free(entry); queue_entry_free(entry);
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router_ack: etcp_send failed ret=%d svc_id=%u rx_seq=%u", ret, rconn->svc_id, rconn->rx_seq);
        return;
    }
    rconn->c_ack_sent++;
    rconn->last_sent_ack_seq = rconn->rx_seq;
    rconn->last_ack_sent_tb  = get_time_tb();
}

// Запланировать отправку ACK: снять idle-таймер, и либо отправить сразу (интервал истёк),
// либо взвести ack_timer на остаток ROUTER_ACK_INTERVAL_TB.
static void router_schedule_ack(struct ETCP_ROUTER_CONN* rconn) {
    if (rconn->idle_ack_timer) {
        uasync_cancel_timeout(rconn->inst->ua, rconn->idle_ack_timer);
        rconn->idle_ack_timer = NULL;
    }
    if (rconn->rx_seq == rconn->last_sent_ack_seq && !rconn->receive_blocked) return;
    uint64_t now = get_time_tb();
    uint64_t elapsed = now - rconn->last_ack_sent_tb;
    if (elapsed >= ROUTER_ACK_INTERVAL_TB) {
        router_ack_do_send(rconn);
        return;
    }
    if (!rconn->ack_timer)
        rconn->ack_timer = uasync_set_timeout(rconn->inst->ua,
            ROUTER_ACK_INTERVAL_TB - (uint32_t)elapsed, rconn, router_ack_timer_cb, "router_ack");
}

// Периодический ACK-таймер: отправить ACK при неподтверждённом rx_seq, иначе взвести idle-таймер.
static void router_ack_timer_cb(void* arg) {
    struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)arg;
    if (rconn->closed) return;
    rconn->ack_timer = NULL;
    if (rconn->rx_seq != rconn->last_sent_ack_seq || rconn->receive_blocked)
        router_ack_do_send(rconn);
    if (rconn->rx_seq != rconn->last_sent_ack_seq || rconn->receive_blocked) {
        if (!rconn->ack_timer)
            rconn->ack_timer = uasync_set_timeout(rconn->inst->ua,
                rconn->receive_blocked ? ROUTER_ACK_BUSY_TB : ROUTER_ACK_INTERVAL_TB, rconn, router_ack_timer_cb, "router_ack");
    } else if (!rconn->idle_ack_timer) {
        rconn->idle_ack_timer = uasync_set_timeout(rconn->inst->ua,
            ROUTER_ACK_IDLE_TB, rconn, router_idle_ack_timer_cb, "router_idle_ack");
    }
}

// Idle-таймер: дослать последний ACK после паузы в приёме (чтобы пир не висел на ретрансмитах).
static void router_idle_ack_timer_cb(void* arg) {
    struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)arg;
    if (rconn->closed) return;
    rconn->idle_ack_timer = NULL;
    if (rconn->rx_seq != rconn->last_sent_ack_seq)
        router_ack_do_send(rconn);
}

// ====================================================================
// Reorder / assembly (аналог etcp_output_try_assembly)
// ====================================================================

// Доставка сервисной кодограммы в едином формате [svc_id][src][dst][rx_flags][group_id][payload].
// Для loopback (dst == self) src и dst оба = self, rx_flags = 0.
static int router_deliver_loopback(struct UTUN_INSTANCE* inst, uint64_t group_id, uint8_t svc_id, struct ll_entry* entry, int mode) {
    if (!router_allowed(inst, group_id, inst->my_ed25519_pubkey, svc_id)) { free_entry(entry); return -1; }
    size_t payload_len = entry->len - 1;
    if (!payload_len || payload_len > UINT16_MAX - ROUTER_SVC_HDR_SIZE || (mode & ~(ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT))) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router loopback: invalid payload svc=%u bytes=%zu mode=%x", svc_id, payload_len, mode);
        free_entry(entry); return -1;
    }
    etcp_recv_fn cb = inst->router_bindings.callbacks[svc_id];
    if (!cb) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router loopback: no handler svc=%u group=%016llx", svc_id, (unsigned long long)group_id);
        free_entry(entry); return -1;
    }
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router loopback: entry allocation failed svc=%u", svc_id); free_entry(entry); return -1; }
    e->dgram = u_malloc(ROUTER_SVC_HDR_SIZE + payload_len);
    if (!e->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router loopback: payload allocation failed svc=%u bytes=%zu", svc_id, payload_len);
        queue_entry_free(e); free_entry(entry); return -1;
    }
    e->dgram[0] = svc_id;
    memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, inst->my_ed25519_pubkey, 32); memcpy(e->dgram + ROUTER_SVC_DST_ED_OFF, inst->my_ed25519_pubkey, 32);
    e->dgram[ROUTER_SVC_FLAGS_OFF] = 0;
    memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &group_id, 8);
    if (payload_len > 0) memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF, entry->dgram + 1, payload_len);
    e->len = ROUTER_SVC_HDR_SIZE + payload_len;
    queue_dgram_free(entry); queue_entry_free(entry);
    struct ETCP_CONN local = {.instance = inst, .peer_node_id = inst->node_id};
    cb(&local, e);
    return 0;
}

// Доставить уже проверенный plaintext из recv_q; криптографические проверки выполнены до постановки в очередь.
static int router_deliver(struct ETCP_ROUTER_CONN* rconn, struct ETCP_CONN* conn,
                           const uint8_t* wire, size_t wire_len) {
    struct UTUN_INSTANCE* inst = rconn->inst;
    etcp_recv_fn cb = inst->router_bindings.callbacks[rconn->svc_id];
    if (!cb) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router_deliver: no handler for svc_id=%u", rconn->svc_id);
        return -1;
    }
    const struct SVC_ROUTE_HDR* hdr = (const struct SVC_ROUTE_HDR*)wire;
    const uint8_t* payload = wire + SVC_ROUTE_HDR_SIZE;
    size_t payload_len = wire_len - SVC_ROUTE_HDR_SIZE;
    uint8_t rx_flags = hdr->flags & (ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED);
    size_t entry_len = ROUTER_SVC_HDR_SIZE + payload_len;
    struct ll_entry* svc_entry = queue_entry_new(0);
    if (!svc_entry) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router_deliver: queue_entry_new failed"); return -1; }
    svc_entry->len   = entry_len;
    svc_entry->dgram = u_malloc(entry_len);
    if (!svc_entry->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router deliver: payload allocation failed"); queue_entry_free(svc_entry); return -1; }
    svc_entry->dgram[0] = rconn->svc_id;
    memcpy(svc_entry->dgram + ROUTER_SVC_SRC_ED_OFF, rconn->remote_ed, 32); memcpy(svc_entry->dgram + ROUTER_SVC_DST_ED_OFF, inst->my_ed25519_pubkey, 32);
    svc_entry->dgram[ROUTER_SVC_FLAGS_OFF] = rx_flags;
    memcpy(svc_entry->dgram + ROUTER_SVC_GROUP_OFF, &rconn->group_id, 8);
    if (payload_len > 0) memcpy(svc_entry->dgram + ROUTER_SVC_PAYLOAD_OFF, payload, payload_len);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "router_deliver: svc_id=%u len=%zu flags=%02x src=%016llx dst=%016llx",
                rconn->svc_id, payload_len, rx_flags, (unsigned long long)rconn->remote_node_id,
                (unsigned long long)inst->node_id);
    rconn->c_pkts_rcvd++;
    cb(conn, svc_entry);
    return 0;
}

// Собрать доставку из recv_q: последовательно доставлять пакеты по возрастанию rx_seq, пока есть следующий.
static void router_try_assembly(struct ETCP_ROUTER_CONN* rconn, struct ETCP_CONN* conn) {
    struct ETCP_CONN local = {.instance = rconn->inst, .peer_node_id = rconn->inst->node_id};
    if (!conn) conn = &local;
    uint32_t next = rconn->rx_seq;
    while (1) {
        if (!router_allowed(rconn->inst, rconn->group_id, rconn->remote_ed, rconn->svc_id)) {
            router_close_and_notify(rconn); return;
        }
        struct ll_entry* entry = queue_find_data_by_index(rconn->recv_q, &next);
        if (!entry) break;
        int (*ready)(const struct ETCP_ROUTER_CONN*, const uint8_t*, size_t) =
            rconn->inst->router_bindings.receive_ready[rconn->svc_id];
        if (ready && !ready(rconn, entry->dgram + SVC_ROUTE_HDR_SIZE, entry->len - SVC_ROUTE_HDR_SIZE)) {
            if (!rconn->receive_blocked) DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE,
                "router receive paused: svc=%u peer=%llx next_seq=%u buffered=%u", rconn->svc_id,
                (unsigned long long)rconn->remote_node_id, next, rconn->recv_q->count);
            rconn->receive_blocked = 1;
            if (!rconn->ack_timer) rconn->ack_timer = uasync_set_timeout(rconn->inst->ua,
                ROUTER_ACK_BUSY_TB, rconn, router_ack_timer_cb, "router_receive_busy");
            return;
        }
        if (rconn->receive_blocked) DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE,
            "router receive resumed: svc=%u peer=%llx next_seq=%u", rconn->svc_id, (unsigned long long)rconn->remote_node_id, next);
        rconn->receive_blocked = 0;
        queue_remove_data(rconn->recv_q, entry);
        uint64_t peer_id = rconn->peer_reset_id;
        int ret = router_deliver(rconn, conn, entry->dgram, entry->len);
        if (ret != 0 || rconn->closed || rconn->peer_reset_id != peer_id) { free_entry(entry); return; }
        rconn->rx_seq = next + 1;
        next++;
        queue_dgram_free(entry);
        queue_entry_free(entry);
    }
}

// ====================================================================
// Очередь приёма входящего трафика (между сетью и recv_q)
// ====================================================================

// Коллбэк incoming_q: переместить пакет в recv_q (с дедупликацией по seq), при совпадении с
// rx_seq — собрать доставку, затем запланировать ACK.
static void router_incoming_q_cb(struct ll_queue* q, void* arg) {
    struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)arg;
    if (rconn->closed) { queue_resume_callback(q); return; }
    struct ll_entry* e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); return; }

    uint32_t seq = *(uint32_t*)e->data;

    if ((int32_t)(seq - rconn->rx_seq) < 0 || queue_find_data_by_index(rconn->recv_q, &seq)) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "router_incoming_q: dup in recv_q seq=%u, dropping", seq);
        queue_dgram_free(e); queue_entry_free(e); queue_resume_callback(q); return;
    }

    struct ll_entry* rq = queue_entry_new(4);
    if (!rq) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router reorder entry allocation failed svc=%u seq=%u", rconn->svc_id, seq);
        queue_dgram_free(e); queue_entry_free(e); queue_resume_callback(q); return;
    }
    *(uint32_t*)rq->data = seq;
    rq->dgram = e->dgram; rq->len = e->len; e->dgram = NULL;
    queue_entry_free(e);

    queue_data_put_with_index(rconn->recv_q, rq);

    if (seq == rconn->rx_seq) router_try_assembly(rconn, rconn->recv_conn);
    if (!rconn->closed && rconn->incoming_q == q) { router_schedule_ack(rconn); queue_resume_callback(q); }
}

// ====================================================================
// Функции приёма etcp_router_recv_cb
// ====================================================================

// Обработать ACK: обновить RTT/jitter, снять подтверждённые inflight-пакеты, при необходимости
// возобновить отправку (снять send_blocked).
static void router_handle_ack(struct UTUN_INSTANCE* inst, struct ETCP_ROUTER_CONN* rconn,
                               uint32_t seq, uint64_t src_node_id, uint16_t ack_ts, int receive_busy) {
    (void)src_node_id;
    if (!rconn) return;
    if ((int32_t)(seq - rconn->tx_sent) > 0) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router ACK outside sent range: peer=%016llx svc=%u ack=%u sent=%u acked=%u",
                   (unsigned long long)src_node_id, rconn->svc_id, seq, rconn->tx_sent, rconn->tx_acked);
        rconn->c_stale_ack++;
        return;
    }
    if ((int32_t)(seq - rconn->tx_acked) < 0) { rconn->c_stale_ack++; return; }
    if (receive_busy != rconn->peer_receive_blocked) DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE,
        "router peer receive busy=%d svc=%u peer=%llx ack=%u", receive_busy, rconn->svc_id, (unsigned long long)src_node_id, seq);
    int resumed = rconn->peer_receive_blocked && !receive_busy;
    rconn->peer_receive_blocked = (uint8_t)receive_busy;
    if (receive_busy) {
        uint64_t now = get_time_tb();
        if (!inst->router_bindings.tx_policy[rconn->svc_id]) rconn->last_ack_changed_tb = now;
        rconn->no_ack_count = 0;
        rconn->retrans_due_tb = now + router_retrans_rto(rconn);
        rconn->retrans_seq = rconn->retrans_end;
        rconn->retrans_started = 0;
        router_cancel_send_waiter(rconn);
        router_retrans_schedule(rconn);
    }
    if (resumed) router_send_kick(rconn);
    if ((int32_t)(seq - rconn->tx_acked) <= 0) { rconn->c_stale_ack++; return; }
    if (ack_ts != 0) {
        uint16_t new_rtt = get_current_timestamp() - ack_ts;
        int d_rtt = (int)new_rtt - (int)rconn->rtt;
        if (d_rtt < 0) d_rtt = -d_rtt;
        int64_t jitter = rconn->rtt_jitter;
        rconn->rtt_jitter = (uint32_t)(jitter + ((int64_t)d_rtt * 65536 - jitter) / 32);
        rconn->rtt = new_rtt;
    }
    rconn->tx_acked = seq;
    rconn->start_sent = 1;
    uint64_t sample_tb = 0;
    while (rconn->inflight_q->head) {
        struct ROUTER_INFLIGHT* inf = (struct ROUTER_INFLIGHT*)rconn->inflight_q->head;
        if ((int32_t)(inf->seq - seq) >= 0) break;
        uint64_t now_us = get_time_us(), elapsed = now_us - inf->enqueued_us;
        if (inf->send_count == 1) {
            sample_tb = get_time_tb() - inf->last_sent_tb;
            if (!sample_tb) sample_tb = 1;
        }
        rconn->delivery_rtt_us = rconn->delivery.packets ? rconn->delivery_rtt_us - rconn->delivery_rtt_us / 8 + elapsed / 8 : elapsed;
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "router delivery svc=%u seq=%u enqueue_ack_us=%llu average_us=%llu attempts=%u",
                    rconn->svc_id, inf->seq, (unsigned long long)elapsed,
                    (unsigned long long)rconn->delivery_rtt_us, inf->send_count);
        route_tx_meter_ack(&rconn->delivery, now_us, inf->ll.len);
        queue_remove_data(rconn->inflight_q, &inf->ll);
        u_free(inf->dgram); u_free(inf);
    }
    // Один образец на накопительный ACK, только от последнего пакета без повторов (Karn).
    if (sample_tb) {
        if (!rconn->retrans_srtt_tb) {
            rconn->retrans_srtt_tb = sample_tb; rconn->retrans_rttvar_tb = sample_tb / 2;
        } else {
            uint64_t srtt = rconn->retrans_srtt_tb;
            uint64_t deviation = sample_tb > srtt ? sample_tb - srtt : srtt - sample_tb;
            rconn->retrans_rttvar_tb = (3 * rconn->retrans_rttvar_tb + deviation) / 4;
            rconn->retrans_srtt_tb = (7 * srtt + sample_tb) / 8;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "router RTO sample: svc=%u peer=%016llx submit_ack_ms=%llu srtt_ms=%llu var_ms=%llu rto_ms=%llu",
                    rconn->svc_id, (unsigned long long)src_node_id, (unsigned long long)(sample_tb / 10),
                    (unsigned long long)(rconn->retrans_srtt_tb / 10), (unsigned long long)(rconn->retrans_rttvar_tb / 10),
                    (unsigned long long)(router_retrans_rto(rconn) / 10));
    }
    rconn->last_ack_changed_tb = get_time_tb();
    rconn->no_ack_count = 0;
    rconn->retrans_seq = rconn->retrans_end; rconn->retrans_started = 0;
    rconn->retrans_due_tb = rconn->last_ack_changed_tb + router_retrans_rto(rconn);
    router_track_inflight_state(rconn);
    router_update_minrtt(rconn);
    router_retrans_schedule(rconn);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "SEND_Q_ACKED: svc_id=%u ack=%u inflight=%d send_q=%d inflight_q=%d from %016llx",
                rconn->svc_id, seq, queue_entry_count(rconn->inflight_q),
                queue_entry_count(rconn->send_q), queue_entry_count(rconn->inflight_q), (unsigned long long)src_node_id);
    rconn->c_ack_recv++;
    rconn->last_dgram_ts = get_current_timestamp();
    rconn->send_blocked = 0;
    router_send_kick(rconn);
}

// Ретранслировать транзитный пакет к dst_node_id: отправить напрямую, при занятости send_input_q —
// поставить в транзитную очередь с backpressure.
static void router_forward_transit(struct UTUN_INSTANCE* inst, struct ll_entry* entry,
                                    struct SVC_ROUTE_HDR* hdr) {
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, hdr->group_id);
    struct ETCP_CONN* next = router_group_next(group, hdr->dst_ed);
    if (!next) {
        int node_count = group && group->nodes ? queue_entry_count(group->nodes) : -1;
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "etcp_router: no route to %016llx svc_id=%u, dropping (group=%016llx nodes=%d)",
                   (unsigned long long)hdr->dst_node_id, hdr->svc_id,
                   (unsigned long long)hdr->group_id, node_count);
        if (group && group->nodes && node_count > 0 && node_count < 10) {
            char list[512]; int pos = 0;
            struct ll_entry* e = group->nodes->head;
            while (e) {
                struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)e;
                pos += snprintf(list + pos, sizeof(list) - pos, "%s%016llx(up=%02x)",
                                pos ? "," : "", (unsigned long long)nq->node_id, nq->conn_up);
                e = e->next;
            }
            DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "etcp_router: group nodes: %s", list);
        }
        free_entry(entry); return;
    }
    struct TRANSIT_QUEUE* tq = transit_queue_find(next, hdr);

    if (!tq && next->send_input_q && queue_entry_count(next->send_input_q) == 0) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "etcp_router: forwarding svc_id=%u → %016llx via %s",
                    hdr->svc_id, (unsigned long long)hdr->dst_node_id, next->log_name);
        uint8_t service = hdr->svc_id; uint64_t destination = hdr->dst_node_id;
        int ret = router_wire_send(next, entry, 0);
        if (ret != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "etcp_router: forward etcp_send failed ret=%d svc_id=%u → %016llx",
                        ret, service, (unsigned long long)destination);
            free_entry(entry);
        }
        return;
    }

    if (!tq) {
        tq = transit_queue_get_or_create(next, hdr);
        if (!tq) { free_entry(entry); return; }
    }
    int was_empty = (queue_entry_count(tq->q) == 0);
    queue_data_put(tq->q, entry);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "etcp_router: queued transit svc_id=%u src=%016llx dst=%016llx q=%d",
                hdr->svc_id, (unsigned long long)hdr->src_node_id,
                (unsigned long long)hdr->dst_node_id, queue_entry_count(tq->q));
    if (was_empty && next->send_input_q)
        transit_queue_wait(tq);
}

// Принять данные согласованной сессии: зафиксировать recv_conn, проверить окно/дубликаты и положить в incoming_q.
static void router_handle_data_packet(struct ETCP_ROUTER_CONN* rconn, struct ETCP_CONN* conn,
                                       uint32_t seq, const uint8_t* wire, size_t wire_len) {
    rconn->recv_conn = conn;
    rconn->last_dgram_ts = get_current_timestamp();

    {// SEQ за пределами окна
        int32_t d = (int32_t)(seq - rconn->rx_seq);
        if (d > (int32_t)rconn->max_inflight || d < -(int32_t)rconn->max_inflight) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router: seq=%u out of bounds, rx_seq=%u (d=%d), dropping",
                        seq, rconn->rx_seq, d);
            rconn->c_oob_dropped++; return;
        }
    }

    if (((int32_t)(rconn->rx_seq - seq) > 0) || queue_find_data_by_index(rconn->recv_q, &seq)) {// дубликат или seq <= rx_seq
        // Сценарий потерянного ACK: пир ретранслит, значит не получил наш ACK — дослать его.
        // router_schedule_ack здесь не годится: после первого ACK rx_seq==last_sent_ack_seq и он выходит рано.
        uint64_t now = get_time_tb();
        if (now - rconn->last_ack_sent_tb >= ROUTER_ACK_INTERVAL_TB) {
            DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "router: dup seq=%u rx_seq=%u, resending ACK", seq, rconn->rx_seq);
            router_send_ack(rconn);
        }
        DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "router: dup seq=%u rx_seq=%u, dropping", seq, rconn->rx_seq);
        rconn->c_dup_dropped++; return;
    }

    if (rconn->incoming_q->count + rconn->recv_q->count >= (unsigned)rconn->max_inflight + 1) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router receive window full: svc=%u peer=%llx next=%u seq=%u buffered=%u",
            rconn->svc_id, (unsigned long long)rconn->remote_node_id, rconn->rx_seq, seq,
            rconn->incoming_q->count + rconn->recv_q->count);
        return;
    }
    struct ll_entry* qe = queue_entry_new(4);
    if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router incoming entry allocation failed svc=%u seq=%u", rconn->svc_id, seq); return; }
    *(uint32_t*)qe->data = seq;
    qe->dgram = u_malloc(wire_len);
    if (!qe->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router incoming payload allocation failed svc=%u seq=%u bytes=%zu", rconn->svc_id, seq, wire_len);
        queue_dgram_free(qe); queue_entry_free(qe); return;
    }
    qe->len = wire_len; memcpy(qe->dgram, wire, wire_len);

    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "router: incoming_q seq=%u rx_seq=%u from %016llx svc_id=%u",
                seq, rconn->rx_seq,
                (unsigned long long)rconn->remote_node_id, rconn->svc_id);
    queue_data_put(rconn->incoming_q, qe);
}

// ====================================================================
// etcp_router_recv_cb — обработчик ETCP_ID_SVC_ROUTE
// ====================================================================

// Входная точка приёма SVC_ROUTE-кодограмм: различает транзит, RST/CLOSE/ACK (пустой payload)
// и data-пакеты, направляя их в router_handle_data_packet.
static void etcp_router_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!entry) return;
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    if (!inst || !entry->dgram || entry->len < ROUTER_FIXED_FIELDS) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router recv: invalid packet len=%u", entry->len);
        free_entry(entry); return;
    }
    size_t body = router_wire_read(conn, entry);
    if (!body) { free_entry(entry); return; }
    struct SVC_ROUTE_HDR* h = (struct SVC_ROUTE_HDR*)entry->dgram;
    if (body - SVC_ROUTE_HDR_SIZE > ROUTER_MAX_ENCODED_BODY) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router receive: oversized body peer=%016llx svc=%u len=%zu max=%u",
                   (unsigned long long)conn->peer_node_id, h->svc_id, body - SVC_ROUTE_HDR_SIZE, (unsigned)ROUTER_MAX_ENCODED_BODY);
        free_entry(entry); return;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, "route RX conn=%s svc=%u flags=%02x seq=%u len=%u src=%016llx dst=%016llx",
                conn->log_name, h->svc_id, h->flags, h->seq, entry->len,
                (unsigned long long)h->src_node_id, (unsigned long long)h->dst_node_id);
    if (memcmp(h->dst_ed, inst->my_ed25519_pubkey, SC_PUBKEY_SIZE)) {
        if (router_path_append(entry, inst->my_ed25519_pubkey, entry->dgram[entry->len - 1]) < 0) {
            inst->router_path_errors++; free_entry(entry); return;
        }
        router_forward_transit(inst, entry, (struct SVC_ROUTE_HDR*)entry->dgram); return;
    }
    entry->len = body;
    enum route_crypto_format format = router_crypto_format(inst, h->group_id);
    struct ETCP_ROUTER_CONN* rconn = etcp_router_conn_lookup(inst, h->group_id, h->src_ed, h->svc_id);
    if (format == ROUTE_CRYPTO_CHAT && !inst->router_bindings.callbacks[h->svc_id] && !rconn) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router receive: unbound CHAT service group=%016llx peer=%016llx svc=%u",
            (unsigned long long)h->group_id, (unsigned long long)h->src_node_id, h->svc_id);
        free_entry(entry); return;
    }
    uint8_t required = router_policy_applies(inst, h->group_id, h->svc_id) ? inst->router_bindings.required_crypto[h->svc_id] : 0;
    if (format == ROUTE_CRYPTO_CHAT) required = ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT;
    uint8_t control_required = format == ROUTE_CRYPTO_CHAT ? required : required & ROUTER_FLAG_SIGNED;
    if ((h->flags & control_required) != control_required) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router receive: insufficient crypto svc=%u peer=%016llx flags=%02x required=%02x",
                   h->svc_id, (unsigned long long)h->src_node_id, h->flags, required);
        free_entry(entry); return;
    }
    if (h->flags & (ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED)) {
        uint8_t* decoded = NULL; size_t len = 0; uint8_t flags = 0;
        uint8_t x[32], ed[32];
        if (router_peer_keys(inst, h->group_id, h->src_node_id, h->src_ed, x, ed)
            || route_crypto_decode(inst, format, x, ed, entry->dgram, entry->len, &decoded, &len, &flags) != 0) {
            if (rconn) rconn->c_sign_fail++;
            free_entry(entry); return;
        }
        queue_dgram_free(entry); entry->dgram = decoded; entry->len = len;
        h = (struct SVC_ROUTE_HDR*)decoded; h->flags |= flags;
    }
    uint8_t control = h->flags & ~(ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED);
    size_t payload_len = entry->len - SVC_ROUTE_HDR_SIZE;
    if (format == ROUTE_CRYPTO_CHAT && payload_len && !inst->router_bindings.callbacks[h->svc_id]) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router receive: DATA for unbound CHAT service group=%016llx peer=%016llx svc=%u",
            (unsigned long long)h->group_id, (unsigned long long)h->src_node_id, h->svc_id);
        free_entry(entry); return;
    }
    if (payload_len && (h->flags & required) != required) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router data crypto rejected: svc=%u flags=%02x required=%02x", h->svc_id, h->flags, required);
        free_entry(entry); return;
    }
    if (!router_allowed(inst, h->group_id, h->src_ed, h->svc_id)) { free_entry(entry); return; }
    if (!h->reset_id || (payload_len && (control || h->challenge))
        || (!payload_len && control != 0 && control != ROUTER_FLAG_START && control != ROUTER_FLAG_RST
            && control != (ROUTER_FLAG_START | ROUTER_FLAG_RST) && control != ROUTER_FLAG_CLOSE && control != ROUTER_FLAG_RECEIVE_BUSY)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router recv: malformed flags=%02x seq=%u len=%zu epoch=%016llx",
                   h->flags, h->seq, payload_len, (unsigned long long)h->reset_id);
        free_entry(entry); return;
    }
    if (!rconn) {
        if (!payload_len && control != ROUTER_FLAG_START) { free_entry(entry); return; }
        rconn = etcp_router_conn_get_key(inst, h->group_id, h->src_ed, h->svc_id);
        if (!rconn) { free_entry(entry); return; }
    }
    if (control == ROUTER_FLAG_START) {
        router_challenge_peer(rconn, conn, h->reset_id);
    } else if (control == ROUTER_FLAG_RST) {
        if (h->peer_reset_id == rconn->reset_id && h->challenge) {
            router_control(rconn, conn, ROUTER_FLAG_START | ROUTER_FLAG_RST, h->reset_id, h->challenge);
            if (h->reset_id != rconn->peer_reset_id) router_challenge_peer(rconn, conn, h->reset_id);
        }
    } else if (control == (ROUTER_FLAG_START | ROUTER_FLAG_RST)) {
        router_confirm_peer(rconn, h);
    } else if (h->peer_reset_id != rconn->reset_id || h->reset_id != rconn->peer_reset_id) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE,
                    "router epoch mismatch: peer=%016llx svc=%u seq=%u flags=%02x received=%llx/%llx expected=%llx/%llx",
                    (unsigned long long)h->src_node_id, h->svc_id, h->seq, h->flags,
                    (unsigned long long)h->reset_id, (unsigned long long)h->peer_reset_id,
                    (unsigned long long)rconn->peer_reset_id, (unsigned long long)rconn->reset_id);
        if (payload_len) router_challenge_peer(rconn, conn, h->reset_id);
    } else if (control == ROUTER_FLAG_CLOSE) {
        router_close_and_notify(rconn);
    } else if (!payload_len) {
        if (!h->challenge) router_handle_ack(inst, rconn, h->seq, h->src_node_id, h->timestamp, control == ROUTER_FLAG_RECEIVE_BUSY);
    } else {
        rconn->last_recv_pkt_ts = h->timestamp;
        rconn->last_recv_pkt_local_tb = get_time_tb(); rconn->last_recv_updated = 1;
        router_handle_data_packet(rconn, conn, h->seq, entry->dgram, entry->len);
    }
    free_entry(entry);
}

// ====================================================================
// Публичное API
// ====================================================================

// Инициализация роутера: создать реестр router_conns, очистить bindings и зарегистрировать
// обработчик ETCP_ID_SVC_ROUTE.
int etcp_router_init(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "NULL instance"); return -1; }
    memset(&inst->router_bindings, 0, sizeof(inst->router_bindings));
    inst->router_conns = queue_new(inst->ua,
        ROUTER_CONN_HASH_SIZE, 0, ROUTER_CONN_KEY_SIZE, "router_conns");
    if (!inst->router_conns) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "queue_new(router_conns) failed");
        return -1;
    }
    int ret = etcp_bind(inst, ETCP_ID_SVC_ROUTE, etcp_router_recv_cb);
    if (ret != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "etcp_bind failed, ret=%d", ret);
        queue_free(inst->router_conns); inst->router_conns = NULL;
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "etcp_router initialized for node %016llx", (unsigned long long)inst->node_id);
    return 0;
}

// Деинициализация: снять обработчик SVC_ROUTE, закрыть все conn и освободить реестр.
void etcp_router_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    etcp_unbind(inst, ETCP_ID_SVC_ROUTE);
    if (inst->router_conns) {
        struct ll_entry* entry;
        while ((entry = inst->router_conns->head) != NULL) {
            struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)entry;
            router_close_and_notify(rconn);
        }
        queue_free(inst->router_conns);
        inst->router_conns = NULL;
    }
    for (struct ll_entry* entry = inst->connections ? inst->connections->head : NULL; entry; entry = entry->next) {
        struct ETCP_CONN* conn = ((struct conn_queue_entry*)entry->data)->conn;
        if (conn) etcp_router_transit_queues_destroy(conn);
    }
    memset(&inst->router_bindings, 0, sizeof(inst->router_bindings));
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "etcp_router destroyed");
}

// Зарегистрировать обработчик; callback=NULL снимает callback, readiness и crypto policy без ошибки отсутствия.
int etcp_router_bind(struct UTUN_INSTANCE* inst, uint8_t svc_id, etcp_recv_fn callback) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "NULL instance"); return -1; }
    if (!callback) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "unbind svc_id=%u", svc_id);
        inst->router_bindings.callbacks[svc_id] = NULL;
        inst->router_bindings.authorize[svc_id] = NULL;
        inst->router_bindings.receive_ready[svc_id] = NULL;
        inst->router_bindings.required_crypto[svc_id] = 0;
        inst->router_bindings.chat_policy[svc_id] = 0;
        return 0;
    }
    if (inst->router_bindings.callbacks[svc_id])
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "overwriting svc_id=%u", svc_id);
    inst->router_bindings.callbacks[svc_id] = callback;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "svc_id=%u → cb=%p", svc_id, (void*)callback);
    return 0;
}

// Снять обработчик сервиса. Возвращает -1 если обработчик не был зарегистрирован.
int etcp_router_unbind(struct UTUN_INSTANCE* inst, uint8_t svc_id) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "NULL instance"); return -1; }
    if (!inst->router_bindings.callbacks[svc_id]) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "svc_id=%u not bound", svc_id);
        return -1;
    }
    inst->router_bindings.callbacks[svc_id] = NULL;
    inst->router_bindings.authorize[svc_id] = NULL;
    inst->router_bindings.receive_ready[svc_id] = NULL;
    inst->router_bindings.required_crypto[svc_id] = 0;
    inst->router_bindings.chat_policy[svc_id] = 0;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "svc_id=%u", svc_id);
    return 0;
}

/* Установить политику исходящего канала отдельно от callback приёма. */
int etcp_router_set_tx_policy(struct UTUN_INSTANCE* inst, uint8_t svc_id, const struct router_tx_policy* policy) {
    if (!inst || (policy && (!policy->ack_timeout_tb || policy->ack_timeout_tb > INT_MAX))) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router TX policy: invalid instance or ACK timeout svc=%u", svc_id);
        return -1;
    }
    inst->router_bindings.tx_policy[svc_id] = policy;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "router TX policy svc=%u live_route=%d ack_timeout_ms=%u", svc_id,
                policy != NULL, policy ? policy->ack_timeout_tb / 10 : 0);
    return 0;
}

int etcp_router_require_crypto(struct UTUN_INSTANCE* inst, uint8_t svc_id, uint8_t flags) {
    if (!inst || !inst->router_bindings.callbacks[svc_id] || (flags & ~(ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED)) ||
        (inst->router_bindings.authorize[svc_id] && !(flags & ROUTER_FLAG_SIGNED))) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router crypto policy invalid inst=%p svc=%u flags=%02x", (void*)inst, svc_id, flags);
        return -1;
    }
    inst->router_bindings.required_crypto[svc_id] = flags;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router crypto policy svc=%u required=%02x", svc_id, flags);
    return 0;
}

int etcp_router_set_authorize(struct UTUN_INSTANCE* inst, uint8_t svc_id,
                              int (*authorize)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32])) {
    if (!inst || !inst->router_bindings.callbacks[svc_id] ||
        (authorize && !(inst->router_bindings.required_crypto[svc_id] & ROUTER_FLAG_SIGNED))) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router access policy requires bound signed service svc=%u", svc_id); return -1;
    }
    inst->router_bindings.authorize[svc_id] = authorize;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router access policy svc=%u enabled=%d", svc_id, authorize != NULL);
    return 0;
}

int etcp_router_set_chat_policy(struct UTUN_INSTANCE* inst, uint8_t svc_id, uint8_t flags,
                               int (*authorize)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32])) {
    if (!inst || !inst->router_bindings.callbacks[svc_id] || !authorize || !(flags & ROUTER_FLAG_SIGNED)
        || (flags & ~(ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED))
        || inst->router_bindings.authorize[svc_id] || inst->router_bindings.required_crypto[svc_id]) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router CHAT policy invalid or already configured svc=%u flags=%02x", svc_id, flags);
        return -1;
    }
    inst->router_bindings.required_crypto[svc_id] = flags;
    inst->router_bindings.authorize[svc_id] = authorize;
    inst->router_bindings.chat_policy[svc_id] = 1;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router CHAT policy svc=%u required=%02x; UTUN unchanged", svc_id, flags);
    return 0;
}

void etcp_router_revalidate_group(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    if (!inst) return;
    for (;;) {
        struct ETCP_ROUTER_CONN* denied = NULL;
        for (struct ll_entry* e = inst->router_conns ? inst->router_conns->head : NULL; e; e = e->next) {
            struct ETCP_ROUTER_CONN* c = (struct ETCP_ROUTER_CONN*)e;
            if (c->group_id == group_id && !router_allowed(inst, group_id, c->remote_ed, c->svc_id)) { denied = c; break; }
        }
        if (!denied) return;
        router_close_and_notify(denied);
    }
}

void etcp_router_set_receive_ready(struct UTUN_INSTANCE* inst, uint8_t svc_id,
                                  int (*ready)(const struct ETCP_ROUTER_CONN*, const uint8_t*, size_t)) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router receive readiness: NULL instance"); return; }
    inst->router_bindings.receive_ready[svc_id] = ready;
}

static void router_receive_pump(void* arg) {
    struct ETCP_ROUTER_CONN* rconn = arg;
    rconn->receive_pump = NULL;
    if (rconn->closed) return;
    // Физический recv_conn мог исчезнуть за время дискового I/O; сервис получает стабильный instance.
    router_try_assembly(rconn, NULL);
    if (!rconn->closed) router_schedule_ack(rconn);
}

void etcp_router_resume_receive(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, uint8_t svc_id) {
    etcp_router_conn_resume_receive(inst ? router_conn_find(inst, group, peer, svc_id) : NULL);
}

void etcp_router_conn_resume_receive(struct ETCP_ROUTER_CONN* rconn) {
    if (!rconn || rconn->closed || !rconn->receive_blocked || rconn->receive_pump) return;
    rconn->receive_pump = uasync_call_soon(rconn->inst->ua, rconn, router_receive_pump);
    if (!rconn->receive_pump) DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router receive resume allocation failed svc=%u", rconn->svc_id);
}

// Отправить сервисный пакет. entry->dgram[0] = svc_id, остальное — payload. Берёт на себя
// освобождение entry в любом исходе. dst==self — loopback-доставка.
static int router_send_entry(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t dst_node_id, const uint8_t* ed,
                             struct ll_entry* entry, int force, int mode) {
    if (!inst || !entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "NULL inst=%p entry=%p", (void*)inst, (void*)entry);
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); } return -1;
    }
    if (!entry->dgram || entry->len < 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "empty entry: dgram=%p len=%u dst=0x%016llx force=%d",
                   (void*)entry->dgram, entry->len, (unsigned long long)dst_node_id, force); queue_dgram_free(entry); queue_entry_free(entry); return -1;
    }
    uint8_t  svc_id      = entry->dgram[0];
    size_t   payload_len = entry->len - 1;

    if (ed ? !memcmp(ed, inst->my_ed25519_pubkey, 32) : dst_node_id == inst->node_id) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "loopback svc_id=%u len=%zu", svc_id, payload_len);
        return router_deliver_loopback(inst, group_id, svc_id, entry, mode);
    }

    if (inst->router_bindings.tx_policy[svc_id] && !router_route_conn(inst, group_id, dst_node_id, svc_id, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router TX rejected: no live path group=%016llx peer=%016llx svc=%u bytes=%zu",
                   (unsigned long long)group_id, (unsigned long long)dst_node_id, svc_id, payload_len);
        free_entry(entry); return -1;
    }
    struct ETCP_ROUTER_CONN* rconn = ed ? etcp_router_conn_get_key(inst, group_id, ed, svc_id) : etcp_router_conn_get(inst, group_id, dst_node_id, svc_id);
    if (!rconn) { queue_dgram_free(entry); queue_entry_free(entry); return -1; }

    int ret = router_enqueue_send(rconn, entry->dgram + 1, payload_len, force, mode);
    queue_dgram_free(entry); queue_entry_free(entry);
    return ret;
}

int etcp_route_send(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t dst_node_id, struct ll_entry* entry, int force, int mode) {
    return router_send_entry(inst, group_id, dst_node_id, NULL, entry, force, mode);
}

int etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], struct ll_entry* entry, int force, int mode) {
    uint8_t x[32];
    if (sc_identity_public_x("router send", ed, x) != SC_OK) { if (entry) free_entry(entry); return -1; }
    return router_send_entry(inst, group_id, sc_derive_node_id_from_pubkey(x), ed, entry, force, mode);
}

// Рестарт локального состояния для peer+svc в группе (перезапуск удалённой стороны): уведомить
// сервис пустой кодограммой и сбросить conn.
void etcp_router_conn_restart(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t remote_node_id, uint8_t svc_id) {
    struct ETCP_ROUTER_CONN* rconn = router_conn_find(inst, group_id, remote_node_id, svc_id);
    if (!rconn) return;

    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router_restart: svc_id=%u remote=%016llx reset_id=%016llx — resetting local state",
               svc_id, (unsigned long long)remote_node_id, (unsigned long long)rconn->reset_id);

    if (rconn->closed) return;
    uint64_t new_id;
    if (router_random_id(&new_id) != 0) { router_close_and_notify(rconn); return; }
    if (router_conn_reset(rconn) != 0) { router_close_and_notify(rconn); return; }
    rconn->reset_id = new_id;
    rconn->pending_peer_id = 0; rconn->pending_challenge = 0; rconn->challenge_started_tb = rconn->challenge_sent_tb = 0;
    router_send_close_to_service(rconn);
    if (!rconn->closed) router_handshake_start(rconn);
}

/* Снимок логической доставки: физические копии inflight второй раз не учитываются. */
static int router_tx_stats(const struct ETCP_ROUTER_CONN* rconn, uint64_t now_us, struct ETCP_ROUTER_TX_STATS* out) {
    if (!out) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router TX snapshot without output"); return -1; }
    if (!rconn || rconn->closed) { memset(out, 0, sizeof(*out)); return 1; }
    struct ETCP_ROUTER_TX_STATS result = {
        .local_epoch = rconn->reset_id, .peer_epoch = rconn->peer_reset_id,
        .queued_bytes = rconn->send_q->total_bytes, .inflight_bytes = rconn->inflight_q->total_bytes,
        .queued_packets = rconn->send_q->count, .inflight_packets = rconn->inflight_q->count,
        .ack_next_seq = rconn->tx_acked,
        .acked_bytes = rconn->delivery.bytes, .acked_packets = rconn->delivery.packets,
        .delivery_rtt_us = rconn->delivery_rtt_us, .last_ack_us = rconn->delivery.packets ? rconn->delivery.last_us : 0,
        .rtt_us = (uint32_t)rconn->rtt * 100,
        .baseline_rtt_us = rconn->minrtt_window_count ? (uint32_t)rconn->minrtt * 100 : 0,
        .ready = rconn->peer_reset_id != 0, .no_route = rconn->no_route,
        .receive_busy = rconn->peer_receive_blocked, .probing = rconn->minrtt_probe
    };
    if (route_tx_meter_window(&rconn->delivery, now_us, &result.window) < 0) return -1;
    if (rconn->send_q->head)
        result.queued_age_us = now_us - ((struct router_pending*)rconn->send_q->head->data)->enqueued_us;
    if (rconn->inflight_q->head)
        result.inflight_age_us = now_us - ((struct ROUTER_INFLIGHT*)rconn->inflight_q->head)->enqueued_us;
    *out = result;
    return 0;
}

int etcp_router_tx_stats(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, uint8_t svc,
                         uint64_t now_us, struct ETCP_ROUTER_TX_STATS* out) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router TX snapshot without instance"); return -1; }
    return router_tx_stats(router_conn_find(inst, group, peer, svc), now_us, out);
}

int etcp_router_tx_stats_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc,
                             uint64_t now_us, struct ETCP_ROUTER_TX_STATS* out) {
    if (!inst || !ed) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router TX snapshot without peer identity"); return -1; }
    return router_tx_stats(etcp_router_conn_lookup(inst, group, ed, svc), now_us, out);
}

void etcp_router_cancel_request(struct router_send_request* h) {
    if (!h || !h->conn) return;
    struct ETCP_ROUTER_CONN *rconn = h->conn;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCPROUTE, "router producer cancelled svc=%u waited_us=%llu",
                rconn->svc_id, (unsigned long long)(get_time_us() - h->since_us));
    rconn->send_request = NULL; memset(h, 0, sizeof(*h));
    if (!rconn->send_q->head && rconn->retrans_seq == rconn->retrans_end) router_cancel_send_waiter(rconn);
    if (!rconn->send_q->head) router_send_watchdog_disarm(rconn);
}

int etcp_router_request_send(struct ETCP_ROUTER_CONN* rconn, struct router_send_request* h,
                             void (*ready)(struct ETCP_ROUTER_CONN*, void*), void* arg) {
    if (!rconn || rconn->closed || !h || !ready) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router producer registration: missing/closed channel, handle or callback"); return -1;
    }
    if ((h->conn && h->conn != rconn) || (rconn->send_request && rconn->send_request != h)
        || (h->conn && (h->ready != ready || h->arg != arg))) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router producer registration conflicts svc=%u", rconn->svc_id); return -1;
    }
    if (h->conn) return 0;
    *h = (struct router_send_request){rconn, ready, arg, get_time_us()}; rconn->send_request = h;
    router_send_kick(rconn); router_send_watchdog_arm(rconn);
    return 0;
}

void etcp_router_conn_on_send_ready(struct ETCP_ROUTER_CONN* rconn, struct queue_waiter_handle* h,
                                    queue_threshold_callback_fn callback, void* arg) {
    if (!h) return;
    if (h->internal || h->call_soon_id) queue_waiter_cancel(h->defer_q, h);
    if (!rconn || rconn->closed || !rconn->send_q) return;
    if (rconn->inst->router_bindings.tx_policy[rconn->svc_id] && !router_send_conn(rconn)) return;
    h->defer_q = rconn->send_q; /* Keep the registration queue across route/session replacement. */
    queue_waiter_wait(rconn->send_q, h, callback, arg);
}

// Отменяем исходную регистрацию по handle, в том числе после смены маршрута.
void etcp_router_cancel_send_ready(struct queue_waiter_handle* h) {
    if (h && (h->internal || h->call_soon_id)) queue_waiter_cancel(h->defer_q, h);
}

int etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN* rconn) {
    if (!rconn || rconn->closed || !rconn->send_q) return 0;
    return rconn->send_q->count <= rconn->send_q->threshold_max_packets
        && (rconn->send_q->threshold_max_bytes == 0 || rconn->send_q->total_bytes <= rconn->send_q->threshold_max_bytes);
}

// ====================================================================
// API seq-подключений
// ====================================================================

// Найти/создать состояние seq-подключения по (group_id, remote_node_id, svc_id): выделяет conn,
// инициализирует случайную эпоху reset_id и очереди.
struct ETCP_ROUTER_CONN* etcp_router_conn_get(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t id, uint8_t svc) {
    if (!inst || !inst->router_conns) return NULL;
    const uint8_t* ed = NULL;
    uint8_t member[SC_PUBKEY_SIZE];
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    if (router_crypto_format(inst, group_id) == ROUTE_CRYPTO_CHAT) {
        const char* channel = topo_groups_channel(inst->topo_groups, group_id);
        if (!group_policy_member_keys(inst, channel, id, 1, NULL, member)) ed = member;
    } else {
        struct TOPO_GROUP_NODE* peer = topo_node_find_by_id(group, id);
        struct TOPO_NODE* node = peer ? peer->identity : topo_node_registry_find(inst->topo_groups, id);
        if (node) ed = node->ed25519_public_key;
        else if (peer && peer->address_indexed) ed = peer->ed25519;
        else {
            struct ETCP_CONN* conn = instance_find_conn(inst, id);
            if (conn) ed = conn->peer_ed25519_pubkey;
        }
    }
    if (!ed) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router channel needs full identity group=%016llx peer=%016llx svc=%u",
                   (unsigned long long)group_id, (unsigned long long)id, svc); return NULL;
    }
    return etcp_router_conn_get_key(inst, group_id, ed, svc);
}

struct ETCP_ROUTER_CONN* etcp_router_conn_get_key(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], uint8_t svc_id) {
    if (!inst || !inst->router_conns) return NULL;
    uint8_t x[32];
    if (sc_identity_public_x("router channel", ed, x) != SC_OK) return NULL;
    uint64_t remote_node_id = sc_derive_node_id_from_pubkey(x);
    if (!router_allowed(inst, group_id, ed, svc_id)) return NULL;
    if (router_crypto_format(inst, group_id) == ROUTE_CRYPTO_CHAT) {
        const char* channel = topo_groups_channel(inst->topo_groups, group_id);
        if (!group_policy_member_key_allowed(inst, channel, x, ed, 0)) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router channel key not admitted group=%016llx peer=%016llx",
                       (unsigned long long)group_id, (unsigned long long)remote_node_id); return NULL;
        }
    }
    struct ETCP_ROUTER_CONN* rconn = etcp_router_conn_lookup(inst, group_id, ed, svc_id);
    if (rconn) return rconn;

    size_t data_size = sizeof(struct ETCP_ROUTER_CONN) - sizeof(struct ll_entry);
    rconn = (struct ETCP_ROUTER_CONN*)queue_entry_new(data_size);
    if (!rconn) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router_conn_get: queue_entry_new failed"); return NULL; }
    rconn->group_id       = group_id;
    rconn->remote_node_id = remote_node_id; memcpy(rconn->remote_ed, ed, 32);
    rconn->svc_id         = svc_id;
    rconn->inst           = inst;
    rconn->last_dgram_ts  = get_current_timestamp();
    if (router_random_id(&rconn->reset_id) != 0) { queue_entry_free(&rconn->ll); return NULL; }
    if (router_conn_reset(rconn) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router_conn_get: queue_new failed");
        router_conn_free_queues(rconn);
        queue_entry_free(&rconn->ll);
        return NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router_conn: new conn group=%016llx remote=%016llx svc_id=%u crypto=%s",
               (unsigned long long)group_id, (unsigned long long)remote_node_id, svc_id,
               router_crypto_format(inst, group_id) == ROUTE_CRYPTO_CHAT ? "encrypted author signature" : "UTUN service mode");
    queue_data_put_with_index(inst->router_conns, &rconn->ll);
    return rconn;
}

// Отправить данные (payload без svc_id) с авто-seq и контролем inflight. mode — биты
// ROUTE_CRYPTO_SIGN / ROUTE_CRYPTO_ENCRYPT для UTUN; CHAT всегда использует обе защиты.
int etcp_router_conn_send(struct ETCP_ROUTER_CONN* rconn,
                          const uint8_t* data, size_t len, int mode) {
    if (!rconn || !data || len == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router_conn_send: invalid args rconn=%p data=%p len=%zu",
                    (void*)rconn, (const void*)data, len);
        return -1;
    }
    if (rconn->inst->router_bindings.tx_policy[rconn->svc_id] && !router_send_conn(rconn)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "router TX rejected: no live path group=%016llx peer=%016llx svc=%u bytes=%zu",
                   (unsigned long long)rconn->group_id, (unsigned long long)rconn->remote_node_id, rconn->svc_id, len);
        return -1;
    }
    return router_enqueue_send(rconn, data, len, 0, mode);
}

// Закрыть seq-подключение (нормальное закрытие с уведомлением сервиса и пира).
void etcp_router_conn_close(struct ETCP_ROUTER_CONN* rconn) {
    router_close_and_notify(rconn);
}

// Сбросить ретрансмиты для всех router_conn к узлу (без коллбэков и без CLOSE — облегчённо,
// для etcp_conn_reinit, чтобы избежать гонки с очисткой ETCP-очередей).
void etcp_router_pause_retrans_for_node(struct UTUN_INSTANCE* inst, const uint8_t ed[32]) {
    if (!inst || !inst->router_conns) return;
    for (struct ll_entry* e = inst->router_conns->head; e; e = e->next) {
        struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)e;
        if (rconn->closed || memcmp(rconn->remote_ed, ed, 32)) continue;
        router_cancel_send_waiter(rconn);
        DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router transport reinit: peer=%016llx svc=%u retained=%d queued=%d acked=%u sent=%u",
                   (unsigned long long)rconn->remote_node_id, rconn->svc_id, queue_entry_count(rconn->inflight_q),
                   queue_entry_count(rconn->send_q), rconn->tx_acked, rconn->tx_sent);
        if (queue_entry_count(rconn->inflight_q) && !rconn->retrans_timer) router_retrans_schedule(rconn);
        router_send_watchdog_arm(rconn);
    }
}

// Закрыть существующие каналы группы к конкретному Ed25519.
void etcp_router_conn_close_all_for_node(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]) {
    if (!inst || !inst->router_conns) return;
    // Один канал на svc_id. Close может снять другие каналы с реестра, но освобождает память отложенно.
    struct ETCP_ROUTER_CONN* to_close[256];
    unsigned count = 0;
    for (struct ll_entry* entry = inst->router_conns->head; entry; entry = entry->next) {
        struct ETCP_ROUTER_CONN* rconn = (struct ETCP_ROUTER_CONN*)entry;
        if (rconn->group_id == group_id && !memcmp(rconn->remote_ed, ed, 32)) to_close[count++] = rconn;
    }
    for (unsigned i = 0; i < count; i++) router_close_and_notify(to_close[i]);
}

/* Service stop: discard every logical channel in a group before its routes disappear.
 * Call after service producers have cancelled their waiters. */
void etcp_router_close_group(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    if (!inst) return;
    /* Notifications may close other channels too; do not retain list cursors across them. */
    while (inst->router_conns) {
        struct ll_entry* entry = inst->router_conns->head;
        while (entry && ((struct ETCP_ROUTER_CONN*)entry)->group_id != group_id) entry = entry->next;
        if (!entry) break;
        router_close_and_notify((struct ETCP_ROUTER_CONN*)entry);
    }
    for (struct ll_entry* e = inst->connections ? inst->connections->head : NULL; e; e = e->next) {
        struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
        if (!conn) continue;
        struct ll_entry* entry = conn->transit_queues ? conn->transit_queues->head : NULL;
        while (entry) {
            struct TRANSIT_QUEUE* transit = (struct TRANSIT_QUEUE*)entry; entry = entry->next;
            if (transit->group_id == group_id) transit_queue_destroy(conn, transit);
        }
    }
}
