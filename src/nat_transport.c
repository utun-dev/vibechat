#include "nat_transport.h"
#include "etcp.h"
#include "eim_nat.h"
#include "utun_instance.h"
#include "config_parser.h"
#include "tun_if.h"
#include "etcp_router.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/ll_queue.h"
#include "../lib/u_async.h"
#include <string.h>
#include <errno.h>

#define NAT_MAINTENANCE_TB 1000
#define NAT_WRITE_RETRY_TB 100

// Один владелец FIFO и ожидания на каждого адресата. Ed25519 начинается с ll.data для хеш-индекса.
struct nat_peer {
    struct ll_entry ll;
    uint8_t ed[32];
    struct UTUN_INSTANCE* inst;
    struct ll_queue* packets;
    struct ll_entry* pending;
    struct queue_waiter_handle waiter;
    uint64_t last_activity;
};

static void nat_peer_send_ready(struct ll_queue* q, void* arg);
static void nat_tun_retry(void* arg);

// Освободить пакет с сохранением его allocator/pool metadata.
static void nat_packet_free(struct ll_entry* packet) {
    if (packet) { queue_dgram_free(packet); queue_entry_free(packet); }
}

// Перегрузка одного получателя не останавливает остальных; диагностировать суммарные потери не чаще раза в секунду.
static void nat_drop(struct UTUN_INSTANCE* inst, struct ll_entry* packet, const uint8_t ed[32], const char* reason) {
    struct nat_transport_ctx* tr = &inst->nat_tr;
    tr->dropped++;
    uint64_t now = get_time_tb();
    if (!tr->last_drop_log || now - tr->last_drop_log >= 10000) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT drop: reason=%s destination=%s total=%llu pending=%zu",
                   reason, sc_public_key_hex(ed, (char[65]){0}), (unsigned long long)tr->dropped, tr->pending_count);
        tr->last_drop_log = now;
    }
    nat_packet_free(packet);
}

// Передать удержанный пакет, когда у роутера есть место; waiter не хранит указатель на NAT mapping.
static void nat_peer_send_ready(struct ll_queue* q, void* arg) {
    (void)q;
    struct nat_peer* peer = arg;
    struct UTUN_INSTANCE* inst = peer->inst;
    struct nat_transport_ctx* tr = &inst->nat_tr;
    if (!peer->pending) return;
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get_key(inst, TOPO_GROUP_UTUN, peer->ed, ETCP_RT_ID_NAT);
    if (!route || route->closed) return; // maintenance повторит после завершения close/reset.
    if (!etcp_router_conn_send_q_has_room(route)) {
        if (!peer->waiter.internal && !peer->waiter.call_soon_id) {
            DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT waiting for router: destination=%s pending=%zu",
                        sc_public_key_hex(peer->ed, (char[65]){0}), tr->pending_count);
            etcp_router_conn_on_send_ready(route, &peer->waiter, nat_peer_send_ready, peer);
        }
        return;
    }
    struct ll_entry* packet = peer->pending;
    size_t ip_len = packet->len - 1;
    peer->pending = NULL;
    tr->pending_count--;
    peer->last_activity = get_time_tb();
    // Роутер синхронно копирует payload и освобождает исходный TUN entry/dgram при любом результате.
    int result = etcp_route_send_key(inst, TOPO_GROUP_UTUN, peer->ed, packet, 0, 0);
    if (result) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT router send failed: destination=%s error=%d ip_bytes=%zu",
                   sc_public_key_hex(peer->ed, (char[65]){0}), result, ip_len);
    }
    queue_resume_callback(peer->packets);
    if (tr->blocked_packet && tr->pending_count < NAT_PENDING_TOTAL) queue_resume_callback(tr->nat_tun->output_queue);
}

// Забрать ровно один пакет из FIFO; до готовности роутера callback остаётся приостановленным.
static void nat_peer_take(struct ll_queue* q, void* arg) {
    struct nat_peer* peer = arg;
    peer->pending = queue_data_get(q);
    if (!peer->pending) { queue_resume_callback(q); return; }
    nat_peer_send_ready(NULL, peer);
}

// Найти/создать FIFO адресата. Выделения требуются только при первом обращении к этому узлу.
static struct nat_peer* nat_peer_get(struct UTUN_INSTANCE* inst, const uint8_t ed[32]) {
    struct nat_transport_ctx* tr = &inst->nat_tr;
    struct nat_peer* peer = (struct nat_peer*)queue_find_data_by_index(tr->peers, ed);
    if (peer) return peer;
    peer = (struct nat_peer*)queue_entry_new(sizeof(*peer) - sizeof(peer->ll));
    if (!peer) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT peer allocation failed ed=%s", sc_public_key_hex(ed, (char[65]){0})); return NULL; }
    memcpy(peer->ed, ed, 32); peer->inst = inst; peer->last_activity = get_time_tb();
    peer->packets = queue_new(inst->ua, 0, 0, 0, "NAT peer packets");
    if (!peer->packets) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT peer queue allocation failed ed=%s", sc_public_key_hex(ed, (char[65]){0}));
        queue_entry_free(&peer->ll);
        return NULL;
    }
    queue_set_callback(peer->packets, nat_peer_take, peer);
    struct ll_queue* packets = peer->packets;
    if (queue_data_put_with_index(tr->peers, &peer->ll) != 0) {
        // put принимает/освобождает peer; отдельная очередь принадлежит вызывающему до успеха.
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT peer index insertion failed ed=%s", sc_public_key_hex(ed, (char[65]){0}));
        queue_free(packets);
        return NULL;
    }
    return peer;
}

// 1 — пакет принят/отброшен, исходный TUN callback можно возобновить; 0 — пакет возвращён в TUN при общем лимите.
static int nat_forward(struct UTUN_INSTANCE* inst, struct ll_entry* packet, const uint8_t ed[32]) {
    struct nat_transport_ctx* tr = &inst->nat_tr;
    packet->dgram[0] = ETCP_RT_ID_NAT;
    if (tr->pending_count >= NAT_PENDING_TOTAL) {
        tr->blocked_packet = packet; memmove(tr->blocked_ed, ed, 32);
        if (queue_data_put_first(tr->nat_tun->output_queue, packet) == 0) return 0;
        tr->blocked_packet = NULL;
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT failed to preserve packet at global backpressure limit");
        return 1;
    }
    struct nat_peer* peer = nat_peer_get(inst, ed);
    if (!peer) { nat_drop(inst, packet, ed, "peer allocation"); return 1; }
    if (queue_entry_count(peer->packets) + (peer->pending != NULL) >= NAT_PENDING_PER_PEER) {
        nat_drop(inst, packet, ed, "per-peer pending limit");
        return 1;
    }
    peer->last_activity = get_time_tb();
    tr->pending_count++;
    if (queue_data_put(peer->packets, packet) != 0) {
        tr->pending_count--;
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT pending queue rejected packet destination=%s", sc_public_key_hex(ed, (char[65]){0}));
    }
    return 1;
}

// Обе роли получают [prefix][IP]; преобразование ответа выполняется один раз до возможного ожидания.
static void nat_tun_output(struct ll_queue* q, void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    struct nat_transport_ctx* tr = &inst->nat_tr;
    struct ll_entry* packet = queue_data_get(q);
    if (!packet) { queue_resume_callback(q); return; }
    if (!packet->dgram || packet->len < 2) {
        nat_drop(inst, packet, 0, "invalid TUN packet");
        queue_resume_callback(q);
        return;
    }
    const uint8_t* ed = inst->nat.table ? NULL : inst->config->global.nat_via_ed;
    if (packet == tr->blocked_packet) {
        ed = tr->blocked_ed; tr->blocked_packet = NULL;
    } else if (!ed) {
        struct eim_nat_entry* mapping = NULL;
        int result = eim_nat_ingress(&inst->nat, packet->dgram + 1, packet->len - 1, &mapping);
        if (result != EIM_NAT_OK || !mapping) {
            nat_drop(inst, packet, 0, "no ingress mapping or invalid packet");
            queue_resume_callback(q);
            return;
        }
        ed = mapping->src_ed;
    }
    if (nat_forward(inst, packet, ed)) queue_resume_callback(q);
}

// Пока один пакет ожидает TUN, последующие остаются в router recv_q без ACK.
static int nat_receive_ready(const struct ETCP_ROUTER_CONN* route, const uint8_t* data, size_t len) {
    (void)data; (void)len;
    return route->inst->nat_tr.initialized && !route->inst->nat_tr.tun_pending;
}

// Повторить запись того же уже преобразованного пакета; mapping мог истечь, повторной трансляции нет.
static void nat_tun_flush(struct UTUN_INSTANCE* inst) {
    struct nat_transport_ctx* tr = &inst->nat_tr;
    struct ll_entry* entry = tr->tun_pending;
    if (!entry) return;
    size_t len = entry->len - ROUTER_SVC_PAYLOAD_OFF;
    const uint8_t* ip = entry->dgram + ROUTER_SVC_PAYLOAD_OFF;
    ssize_t written = tun_write(tr->nat_tun, ip, len);
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (inst->nat.table && !tr->tun_pending_port) {
            uint16_t port;
            memcpy(&port, ip + (ip[0] & 15) * 4 + (ip[9] == IPPROTO_ICMP_UINT8 ? 4 : 0), 2);
            port = ntohs(port);
            if (eim_nat_hold(&inst->nat, port)) {
                nat_drop(inst, entry, 0, "cannot hold outgoing mapping"); tr->tun_pending = NULL;
                return;
            }
            tr->tun_pending_port = port;
            DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT TUN blocked: held mapping external=%u", port);
        }
        if (!tr->tun_retry_timer) tr->tun_retry_timer = uasync_set_timeout(inst->ua, NAT_WRITE_RETRY_TB, inst, nat_tun_retry, "nat_tun_retry");
        if (!tr->tun_retry_timer) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT TUN retry allocation failed; maintenance will retry");
        return;
    }
    if (tr->tun_pending_port) {
        eim_nat_release(&inst->nat, tr->tun_pending_port, written >= 0);
        tr->tun_pending_port = 0;
    }
    if (tr->tun_retry_timer) {
        uasync_cancel_timeout(inst->ua, tr->tun_retry_timer);
        tr->tun_retry_timer = NULL;
    }
    nat_packet_free(entry); tr->tun_pending = NULL;
    if (inst->router_conns) {
        for (struct ll_entry* e = inst->router_conns->head; e; e = e->next) {
            struct ETCP_ROUTER_CONN* route = (struct ETCP_ROUTER_CONN*)e;
            if (route->svc_id == ETCP_RT_ID_NAT && route->group_id == TOPO_GROUP_UTUN)
                etcp_router_conn_resume_receive(route);
        }
    }
}

static void nat_tun_retry(void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    inst->nat_tr.tun_retry_timer = NULL;
    nat_tun_flush(inst);
}

// Router передаёт владение entry; CLOSE/reset не содержит IP и не меняет таблицу.
static void nat_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!conn || !conn->instance || !entry || !entry->dgram || entry->len < ROUTER_SVC_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT malformed router delivery");
        nat_packet_free(entry);
        return;
    }
    struct UTUN_INSTANCE* inst = conn->instance;
    struct nat_transport_ctx* tr = &inst->nat_tr;
    if (!tr->initialized || entry->len == ROUTER_SVC_HDR_SIZE) { nat_packet_free(entry); return; }
    const uint8_t* source = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    uint64_t group;
    memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    if (group != TOPO_GROUP_UTUN || (!inst->nat.table && memcmp(source, inst->config->global.nat_via_ed, 32))) {
        nat_drop(inst, entry, source, "unexpected source/group");
        return;
    }
    size_t len = entry->len - ROUTER_SVC_PAYLOAD_OFF;
    if (inst->nat.table) {
        int result = eim_nat_egress(&inst->nat, entry->dgram + ROUTER_SVC_PAYLOAD_OFF, len, source, conn);
        if (result != EIM_NAT_OK) { nat_drop(inst, entry, source, "egress invalid or unsupported packet"); return; }
    } else {
        uint16_t header_len;
        if (eim_nat_validate_packet(entry->dgram + ROUTER_SVC_PAYLOAD_OFF, len, &header_len) != EIM_NAT_OK) {
            nat_drop(inst, entry, source, "invalid provider IPv4 packet");
            return;
        }
    }
    if (tr->tun_pending) { nat_drop(inst, entry, source, "concurrent TUN delivery"); return; } // Возможен только loopback.
    tr->tun_pending = entry;
    nat_tun_flush(inst);
}

// Освободить FIFO и waiter до уничтожения TUN-пула; вызывается только вне callbacks этого peer.
static void nat_peer_free(struct nat_peer* peer) {
    struct UTUN_INSTANCE* inst = peer->inst;
    if (peer->waiter.internal || peer->waiter.call_soon_id) queue_waiter_cancel(peer->waiter.defer_q, &peer->waiter);
    if (peer->pending) { nat_packet_free(peer->pending); inst->nat_tr.pending_count--; }
    struct ll_entry* packet;
    while ((packet = queue_data_get(peer->packets))) { nat_packet_free(packet); inst->nat_tr.pending_count--; }
    queue_free(peer->packets);
    queue_entry_free(&peer->ll);
}

// Периодическая работа не входит в обработку пакета. Отсутствующие после reset waiters восстанавливаются.
static void nat_maintenance(void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    struct nat_transport_ctx* tr = &inst->nat_tr;
    tr->maintenance_timer = NULL;
    uint64_t now = get_time_tb();
    if (inst->nat.table) eim_nat_expire(&inst->nat, now, 128);
    for (struct ll_entry* e = tr->peers->head; e;) {
        struct ll_entry* next = e->next;
        struct nat_peer* peer = (struct nat_peer*)e;
        if (peer->pending && !peer->waiter.internal && !peer->waiter.call_soon_id) nat_peer_send_ready(NULL, peer);
        if (!peer->pending && !queue_entry_count(peer->packets) && now >= peer->last_activity && now - peer->last_activity >= 600000) {
            queue_remove_data(tr->peers, e);
            nat_peer_free(peer);
        }
        e = next;
    }
    nat_tun_flush(inst);
    tr->maintenance_timer = uasync_set_timeout(inst->ua, NAT_MAINTENANCE_TB, inst, nat_maintenance, "nat_maintenance");
    if (!tr->maintenance_timer) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT maintenance scheduling failed");
}

// Все ресурсы создаются в потоке ядра; при любой ошибке инициализация откатывает только свои ресурсы.
int nat_transport_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->config) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT init requires instance/config"); return -1; }
    struct nat_transport_ctx* tr = &inst->nat_tr;
    struct global_config* config = &inst->config->global;
    memset(tr, 0, sizeof(*tr));
    if (!config->nat_enabled) return 0;
    if (!memcmp(config->nat_via_ed, (const uint8_t[32]){0}, 32)) {
        if (eim_nat_init_ctx(&inst->nat, config)) return -1;
    } else inst->nat.initialized = 1;
    const struct IP* ip = config->nat_tun_ip.family == AF_INET ? &config->nat_tun_ip : &config->tun_ip;
    if (ip->family != AF_INET) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT requires IPv4 TUN address"); goto failed; }
    const char* name = config->nat_tun_ifname[0] ? config->nat_tun_ifname : "tun_nat";
    tr->nat_tun = tun_init_nat(inst->ua, name, ip_to_str(&ip->addr.v4, AF_INET).str, TUN_MTU_DEFAULT, config->tun_test_mode);
    if (!tr->nat_tun) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT TUN creation failed name=%s", name); goto failed; }
    tr->peers = queue_new(inst->ua, 256, 0, 32, "NAT peers");
    if (!tr->peers) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT peer index allocation failed"); goto failed; }
    if (etcp_router_bind(inst, ETCP_RT_ID_NAT, nat_receive)) goto failed;
    tr->initialized = 1;
    etcp_router_set_receive_ready(inst, ETCP_RT_ID_NAT, nat_receive_ready);
    queue_set_callback(tr->nat_tun->output_queue, nat_tun_output, inst);
    tr->maintenance_timer = uasync_set_timeout(inst->ua, NAT_MAINTENANCE_TB, inst, nat_maintenance, "nat_maintenance");
    if (!tr->maintenance_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT maintenance timer allocation failed");
        nat_transport_destroy(inst);
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT %s initialized TUN=%s peer_limit=%u total_limit=%u",
               inst->nat.table ? "provider" : "client", name, NAT_PENDING_PER_PEER, NAT_PENDING_TOTAL);
    return 0;
failed:
    if (tr->peers) queue_free(tr->peers);
    if (tr->nat_tun) tun_close(tr->nat_tun);
    eim_nat_destroy_ctx(&inst->nat);
    memset(tr, 0, sizeof(*tr));
    return -1;
}

// Отменить производителей/таймеры и вернуть удержанные TUN-пакеты до уничтожения их пула.
void nat_transport_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->nat_tr.initialized) return;
    struct nat_transport_ctx* tr = &inst->nat_tr;
    if (tr->maintenance_timer) uasync_cancel_timeout(inst->ua, tr->maintenance_timer);
    if (tr->tun_retry_timer) uasync_cancel_timeout(inst->ua, tr->tun_retry_timer);
    etcp_router_unbind(inst, ETCP_RT_ID_NAT);
    size_t cancelled = tr->pending_count;
    struct ll_entry* peer;
    while ((peer = queue_data_get(tr->peers))) nat_peer_free((struct nat_peer*)peer);
    queue_free(tr->peers);
    // После отмены producers закрыть только каналы NAT; остальные сервисы ядра продолжают работать.
    while (inst->router_conns) {
        struct ETCP_ROUTER_CONN* route = NULL;
        for (struct ll_entry* e = inst->router_conns->head; e; e = e->next) {
            struct ETCP_ROUTER_CONN* candidate = (struct ETCP_ROUTER_CONN*)e;
            if (candidate->svc_id == ETCP_RT_ID_NAT) { route = candidate; break; }
        }
        if (!route) break;
        etcp_router_conn_close(route);
    }
    if (tr->tun_pending_port) eim_nat_release(&inst->nat, tr->tun_pending_port, 0);
    nat_packet_free(tr->tun_pending);
    tun_close(tr->nat_tun);
    eim_nat_destroy_ctx(&inst->nat);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT destroyed: cancelled=%zu dropped=%llu", cancelled, (unsigned long long)tr->dropped);
    memset(tr, 0, sizeof(*tr));
}
