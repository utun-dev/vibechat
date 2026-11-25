/*
 * chat_status.c — сбор статуса (NTP + ETCP connections) и отправка
 *
 * Вынесено из chat_core.c для уменьшения размера модуля.
 */

#include "chat_core_priv.h"
#include "chat_event.h"
#include "chat_member.h"
#include "chat_member_read.h"
#include "chat_network_stats.h"
#include "../member_files/protocol.h"

#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../../lib/ll_queue.h"
#include "../../lib/mem.h"
#include "../../lib/strbuf.h"
#include "../../lib/db_async.h"
#include <limits.h>
#include "../../lib/platform_compat.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_connections.h"
#include "../transport_layer/stcp_link.h"
#include "../transport_layer/etcp_link_io.h"
#include "../transport_layer/pkt_normalizer.h"
#include "../routing_layer/topo_node.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/etcp_router.h"
#include <string.h>

struct address_profile_text { char text[80]; };

static struct address_profile_text link_address_profile_text(const struct ETCP_LINK *link) {
    static const struct address_profile unknown = {0};
    const struct address_profile *profile = &unknown;
    const struct TOPO_NODE *peer = topo_node_registry_lookup(link->etcp->instance->topo_groups, link->etcp->peer_node_id,
        link->etcp->crypto_ctx.peer_public_key);
    uint8_t socket_id = link->remote_socket_id;
    /* У входящего TCP source port относится к соединению, не к listener пира. */
    if (link->is_tcp && link->is_server) peer = NULL;
    if (peer && link->is_tcp && link->remote_addr.ss_family == AF_INET) {
        const struct sockaddr_in *target = (const struct sockaddr_in *)&link->remote_addr;
        const struct TOPO_ADDR4 *address = peer->v4_addrs;
        while (address && (!(address->protocol & TOPO_PROTO_TCP) || address->port != ntohs(target->sin_port) ||
                          memcmp(address->addr, &target->sin_addr, 4))) address = address->next;
        if (address) socket_id = address->socket_id; else peer = NULL;
    } else if (peer && link->is_tcp && link->remote_addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *target = (const struct sockaddr_in6 *)&link->remote_addr;
        const struct TOPO_ADDR6 *address = peer->v6_addrs;
        while (address && (!(address->protocol & TOPO_PROTO_TCP) || address->port != ntohs(target->sin6_port) ||
                          memcmp(address->addr, &target->sin6_addr, 16))) address = address->next;
        if (address) socket_id = address->socket_id; else peer = NULL;
    }
    if (peer && link->remote_addr.ss_family == AF_INET) {
        for (const struct TOPO_SOCKMETA4 *meta = peer->v4_sock_meta; meta; meta = meta->next)
            if (meta->id == socket_id) { profile = &meta->address_profile; break; }
    } else if (peer && link->remote_addr.ss_family == AF_INET6) {
        for (const struct TOPO_SOCKMETA6 *meta = peer->v6_sock_meta; meta; meta = meta->next)
            if (meta->id == socket_id) { profile = &meta->address_profile; break; }
    }
    struct address_profile_text result;
    snprintf(result.text, sizeof(result.text), "%s/%s (%s)", address_mapping_name(profile->mapping),
             address_inbound_name(profile->inbound), address_profile_source_name(profile->source));
    return result;
}

static void chat_core_collect_status(struct UTUN_INSTANCE* inst) {
    struct strbuf text = strbuf_new();

    if (!inst || !CC(inst) || !CC(inst)->initialized) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "status snapshot: chat unavailable");
        if (strbuf_addf(&text, "uTun not initialized\n") < 0) goto failed;
        goto post;
    }

    struct uasync_load load;
    if (uasync_get_load(inst->ua, &load) == ERR_OK && load.valid) {
        if (strbuf_addf(&text, "uasync CPU: %.1f%%, average %.1f%% (window %.0f ms, age %.0f ms)\n",
                        load.current_percent, load.average_percent, load.window_us / 1000.0,
                        (get_time_us() - load.sample_time_us) / 1000.0) < 0) goto failed;
    } else if (strbuf_addf(&text, "uasync CPU: unavailable\n") < 0) goto failed;

    struct NTP_TIME* ntp = &inst->ntp;
    if (strbuf_addf(&text, "=== NTP ===\nEnabled: %s\nSynced: %s\nOffset: %.1f s\n",
                   ntp->enabled ? "yes" : "no", ntp->synced ? "yes" : "no", ntp->offset_us / 1000000.0) < 0) goto failed;
    if (ntp->server_count > 0 && ntp->servers) {
        if (strbuf_addf(&text, "Servers: %d\n", ntp->server_count) < 0) goto failed;
        for (int i = 0; i < ntp->server_count && ntp->servers[i]; i++)
            if (strbuf_addf(&text, "  [%d] %s%s\n", i, ntp->servers[i],
                            i == ntp->server_current ? " (current)" : "") < 0) goto failed;
    }
    if (ntp->synced && ntp->last_sync_tb > 0) {
        uint64_t now_tb = get_time_tb();
        uint64_t ago_sec = (now_tb - ntp->last_sync_tb) / 10000;
        if (strbuf_addf(&text, "Last sync: %llu sec ago\n", (unsigned long long)ago_sec) < 0) goto failed;
    }

    int conn_count = 0;
    struct ll_entry* entry = inst->connections ? inst->connections->head : NULL;
    while (entry) { conn_count++; entry = entry->next; }
    if (strbuf_addf(&text, "=== Connections (%d) ===\n", conn_count) < 0) goto failed;

    uint64_t my_id = inst->node_id;
    entry = inst->connections ? inst->connections->head : NULL;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        if (!ce || !ce->conn) { entry = entry->next; continue; }

        uint64_t pid = ce->peer_node_id;
        struct ETCP_CONN* conn = ce->conn;

        int link_count = 0;
        struct ETCP_LINK* tl = conn->links;
        while (tl) { link_count++; tl = tl->next; }

        const char* init_str = conn->initialized ? "" : "(!init)";

        char myhex[5], peerhex[5];
        snprintf(myhex, sizeof(myhex), "%04llX", (unsigned long long)(my_id & 0xFFFF));
        snprintf(peerhex, sizeof(peerhex), "%04llX", (unsigned long long)(pid & 0xFFFF));

        char peername[64];
        chat_core_get_node_name(inst, conn->crypto_ctx.peer_public_key, peername, sizeof(peername));

        if (peername[0]) {
            if (strbuf_addf(&text, "[%s]→[%s] %s  ETCP:%s%s(%dL)\n",
                            myhex, peerhex, peername,
                            conn->links_up ? "UP" : "DOWN", init_str, link_count) < 0) goto failed;
        } else {
            if (strbuf_addf(&text, "[%s]→[%s]  ETCP:%s%s(%dL)\n",
                            myhex, peerhex,
                            conn->links_up ? "UP" : "DOWN", init_str, link_count) < 0) goto failed;
        }

        int link_idx = 0;
        struct ETCP_LINK* link = conn->links;
        while (link) {
            char rtt_str[32]; rtt_str[0] = '\0';
            if (link->rtt_last > 0) snprintf(rtt_str, sizeof(rtt_str), " / rtt=%ums", link->rtt_last / 10);
            if (strbuf_addf(&text, "  LINK#%d: %s %s %s NAT=%s%s\n",
                            link_idx,
                            link->is_tcp ? "TCP" : "UDP",
                            link->conn ? link->conn->name : "?",
                            link->link_status ? "UP" : "DOWN",
                            link_address_profile_text(link).text,
                            rtt_str) < 0) goto failed;
            link = link->next; link_idx++;
        }

        entry = entry->next;
    }

post:
    if (text.len > INT_MAX) goto failed;
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT, "status snapshot: bytes=%zu", text.len);
    chat_event_post(inst, CHAT_EVT_STATUS_REFRESH, (const uint8_t*)strbuf_str(&text), (int)text.len);
    strbuf_free(&text);
    return;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "status snapshot: formatting/allocation failed after %zu bytes", text.len);
    strbuf_free(&text);
}

/* ── Бинарный список соединений: [count:2][entry:46B]* ── */
/* entry: peer_node_id:8 flags:1 link_count:1 rtt:2 inflight_kb:2 name:32 */
#define CONN_LIST_ENTRY_SIZE 46

static void collect_conn_list(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst) || !CC(inst)->initialized || !inst->connections) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "connection list: chat/connections unavailable"); return;
    }

    uint16_t count = 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry && count < 250) { count++; entry = entry->next; }
    if (entry) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "connection list limited to250 entries");

    size_t buf_sz = 2 + (size_t)count * CONN_LIST_ENTRY_SIZE;
    uint8_t* buf = (uint8_t*)u_malloc(buf_sz);
    if (!buf) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "connection list allocation failed bytes=%zu", buf_sz); return; }

    uint16_t* hdr = (uint16_t*)buf;
    *hdr = count;
    uint8_t* p = buf + 2;

    entry = inst->connections->head;
    for (uint16_t i = 0; i < count && entry; i++, entry = entry->next) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        if (!ce || !ce->conn) { memset(p, 0, CONN_LIST_ENTRY_SIZE); p += CONN_LIST_ENTRY_SIZE; continue; }
        struct ETCP_CONN* conn = ce->conn;

        memcpy(p, &ce->peer_node_id, 8); p += 8;

        uint8_t flags = 0;
        if (conn->links_up) flags |= 1;
        if (conn->initialized) flags |= 2;
        if (conn->links) {
            if (!conn->links->is_server) flags |= 4;
            else flags |= 8;
        }
        *p++ = flags;

        uint8_t lc = 0;
        struct ETCP_LINK* tl = conn->links; while (tl && lc < UINT8_MAX) { lc++; tl = tl->next; }
        *p++ = lc;

        uint16_t rtt = 0;
        if (conn->links) rtt = conn->links->rtt_last;
        memcpy(p, &rtt, 2); p += 2;

        uint64_t inflight = conn->unacked_bytes / 1024;
        uint16_t inflight_kb = inflight > UINT16_MAX ? UINT16_MAX : (uint16_t)inflight;
        memcpy(p, &inflight_kb, 2); p += 2;

        char name[32]; memset(name, 0, 32);
        chat_core_get_node_name(inst, conn->crypto_ctx.peer_public_key, name, 31);
        if (!name[0]) snprintf(name, 32, "%016llX", (unsigned long long)ce->peer_node_id);
        memcpy(p, name, 32); p += 32;
    }

    chat_event_post(inst, CHAT_EVT_CONN_LIST, buf, (int)buf_sz);
    u_free(buf);
}

/* ── Полный текстовый дамп одного соединения ── */
/* Читаемые имена основных сервисов; числовой svc выводится для каждого, включая неизвестные. */
static const char* router_service_name(uint8_t svc) {
    switch (svc) {
        case ETCP_RT_ID_CALL_AUDIO: return "call audio";
        case ETCP_RT_ID_CALL: return "call signaling";
        case ETCP_RT_ID_CALL_VIDEO: return "call video";
        case ETCP_RT_ID_CALL_VIDEO_CONTROL: return "video control";
        case ETCP_RT_ID_DM: return "DM";
        case ETCP_RT_ID_DM_MEDIA: return "DM media";
        case ETCP_RT_ID_FILE_TRANSFER: return "file transfer";
        case ETCP_RT_ID_MEMBER_FILES: return "member files";
        case ETCP_RT_ID_CONN_MGR: return "connection manager";
        default: return "service";
    }
}

/* Только чтение в uasync: сессии выбранного пира, текущего TX-пути или последнего RX-пути и его transit FIFO.
 * Байты — счётчики очередей, не wire size. Ошибка форматирования возвращается сборщику для общего ERROR. */
static int append_router_metrics(struct strbuf* text, struct UTUN_INSTANCE* inst, struct ETCP_CONN* conn) {
    if (strbuf_addf(text, "\n--- Router queues ---\n"
        "Scope: peer / current TX hop / last RX hop\n"
        "send_q: before ETCP; wait_ack: service ACK pending (may still be queued in ETCP)\n") < 0) return -1;
    unsigned sessions = 0;
    for (struct ll_entry* e = inst->router_conns ? inst->router_conns->head : NULL; e; e = e->next) {
        struct ETCP_ROUTER_CONN* rc = (struct ETCP_ROUTER_CONN*)e;
        struct ETCP_ROUTER_PATH path = etcp_router_get_path(inst, rc->group_id, rc->remote_node_id, rc->svc_id);
        if (rc->remote_node_id != conn->peer_node_id && rc->recv_conn != conn && path.next_hop_node_id != conn->peer_node_id) continue;
        sessions++;
        if (strbuf_addf(text, "\ngroup=%016llX peer=%016llX svc=%u (%s)\n"
            "ready=%u no_route=%u send_blocked=%u peer_busy=%u recv_blocked=%u transport_wait=%u\n"
            "RTT=%.1fms inflight_limit=%u retrans=%u\n",
            (unsigned long long)rc->group_id, (unsigned long long)rc->remote_node_id, rc->svc_id, router_service_name(rc->svc_id),
            rc->peer_reset_id != 0, rc->no_route, rc->send_blocked, rc->peer_receive_blocked, rc->receive_blocked,
            rc->send_waiter_q != NULL, rc->rtt / 10.0, rc->inflight_limit, rc->c_retrans_done) < 0) return -1;
        struct ll_queue* queues[] = {rc->send_q, rc->inflight_q, rc->incoming_q, rc->recv_q};
        const char* names[] = {"send_q", "wait_ack", "incoming_q", "recv_q"};
        for (unsigned i = 0; i < 4; i++) {
            if (strbuf_addf(text, "%s: %zuB/%dp%s", names[i], queues[i] ? queue_total_bytes(queues[i]) : 0,
                queue_entry_count(queues[i]), i % 2 ? "\n" : "  ") < 0) return -1;
        }
    }
    if (strbuf_addf(text, "sessions: %u\n", sessions) < 0) return -1;
    size_t transit_bytes = 0;
    unsigned transit_packets = 0;
    for (struct ll_entry* e = conn->transit_queues ? conn->transit_queues->head : NULL; e; e = e->next) {
        struct TRANSIT_QUEUE* tq = (struct TRANSIT_QUEUE*)e;
        size_t bytes = tq->q ? queue_total_bytes(tq->q) : 0;
        unsigned packets = queue_entry_count(tq->q);
        transit_bytes += bytes; transit_packets += packets;
        if (strbuf_addf(text, "transit group=%016llX src=%016llX dst=%016llX: %zuB/%up\n",
            (unsigned long long)tq->group_id, (unsigned long long)tq->src_node_id,
            (unsigned long long)tq->dst_node_id, bytes, packets) < 0) return -1;
    }
    if (strbuf_addf(text, "transit total: %zuB/%up\n", transit_bytes, transit_packets) < 0) return -1;
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT, "connection router metrics: peer=%016llX sessions=%u transit=%zuB/%up",
                (unsigned long long)conn->peer_node_id, sessions, transit_bytes, transit_packets);
    return 0;
}

static void collect_conn_metrics(struct UTUN_INSTANCE* inst, uint64_t peer_node_id) {
    struct strbuf text = strbuf_new();

    if (!inst || !CC(inst) || !CC(inst)->initialized || !inst->connections) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "connection metrics: chat/connections unavailable peer=%016llX",
                   (unsigned long long)peer_node_id);
        if (strbuf_addf(&text, "uTun not initialized\n") < 0) goto failed;
        goto post;
    }

    struct ETCP_CONN* conn;
    int status = instance_lookup_conn(inst, peer_node_id, &conn);
    if (!conn) {
        const char* reason = status == -1 ? "has null data" : status == -2 ? "is ambiguous" : "not found";
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "connection metrics: peer=%016llX %s", (unsigned long long)peer_node_id, reason);
        if (strbuf_addf(&text, "Connection %016llX %s\n", (unsigned long long)peer_node_id, reason) < 0) goto failed;
        goto post;
    }
    uint64_t my_id = inst->node_id;

    char peername[64]; chat_core_get_node_name(inst, conn->crypto_ctx.peer_public_key, peername, sizeof(peername));
    if (!peername[0]) snprintf(peername, sizeof(peername), "%016llX", (unsigned long long)peer_node_id);

    int is_server = conn->links ? conn->links->is_server : -1;
    const char* role_str = is_server < 0 ? "none" : (is_server ? "server" : "client");

    /* Count links */
    int link_count = 0, links_up_count = 0;
    struct ETCP_LINK* tl = conn->links; while (tl) { if (tl->link_status) links_up_count++; link_count++; tl = tl->next; }

    if (strbuf_addf(&text,
        "=== PEER: %04llX\u2192%04llX [%s] ===\n"
        "Role: %s\n"
        "Status: %s  Links: %d/%d  Initialized: %s  MTU: %d\n"
        "Reinit: %u  Reset: %u  Tx_state: %d\n"
        "\n--- ETCP ---\n"
        "RTT last/avg10: %.1f/%.1f ms   Jitter: %.1f ms\n"
        "Bytes sent: %u   Retrans: %u   ACKs: %u\n"
        "Unacked bytes: %u   Max inflight: %u\n"
        "RX dup: %u   TX dup: %u\n"
        "IDs: next_tx=%u last_rx=%u last_del=%u rx_ack_till=%u\n"
        "\n--- Queues ---\n",
        (unsigned long long)(my_id & 0xFFFF), (unsigned long long)(peer_node_id & 0xFFFF), peername,
        role_str,
        conn->links_up ? "UP" : "DOWN", links_up_count, link_count,
        conn->initialized ? "yes" : "no", conn->mtu,
        conn->reinit_count, conn->reset_count, conn->tx_state,
        conn->rtt_last / 10.0f, conn->rtt_avg_10 / 10.0f, conn->jitter / 10.0f,
        conn->bytes_sent_total, conn->retransmissions_count, conn->ack_packets_count,
        conn->unacked_bytes, conn->max_inflight,
        conn->rx_dup_count, conn->tx_dup_count,
        conn->next_tx_id, conn->last_rx_id, conn->last_delivered_id, conn->rx_ack_till) < 0) goto failed;

    if (conn->input_queue && strbuf_addf(&text, "input_q: %uB/%dp  ",
        (unsigned)queue_total_bytes(conn->input_queue), queue_entry_count(conn->input_queue)) < 0) goto failed;
    if (conn->input_send_q && strbuf_addf(&text, "send_q: %uB/%dp  ",
        (unsigned)queue_total_bytes(conn->input_send_q), queue_entry_count(conn->input_send_q)) < 0) goto failed;
    if (conn->input_wait_ack && strbuf_addf(&text, "wait_ack: %uB/%dp\n",
        (unsigned)queue_total_bytes(conn->input_wait_ack), queue_entry_count(conn->input_wait_ack)) < 0) goto failed;
    unsigned ack_bytes = 0, ack_packets = 0;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
        ack_bytes += (unsigned)queue_total_bytes(link->ack_q); ack_packets += queue_entry_count(link->ack_q);
    }
    if (strbuf_addf(&text, "ack_q: %uB/%up  ", ack_bytes, ack_packets) < 0) goto failed;
    if (conn->recv_q && strbuf_addf(&text, "recv_q: %uB/%dp  ",
        (unsigned)queue_total_bytes(conn->recv_q), queue_entry_count(conn->recv_q)) < 0) goto failed;
    if (conn->output_queue && strbuf_addf(&text, "out_q: %uB/%dp\n",
        (unsigned)queue_total_bytes(conn->output_queue), queue_entry_count(conn->output_queue)) < 0) goto failed;

    if (append_router_metrics(&text, inst, conn) < 0) goto failed;

    if (strbuf_addf(&text, "\n--- ACK Debug ---\n"
        "hit_inf=%u hit_sndq=%u miss=%u link_wait=%u\n",
        conn->cnt_ack_hit_inf, conn->cnt_ack_hit_sndq, conn->cnt_ack_miss, conn->cnt_link_wait) < 0) goto failed;

    /* Normalizer */
    if (conn->normalizer) {
        struct PKTNORM* pn = conn->normalizer;
        if (strbuf_addf(&text,
            "\n--- Normalizer ---\n"
            "input_q: %zuB/%dp  output_q: %zuB/%dp\n"
            "in: %llu pkts / %llu B   out: %llu pkts / %llu B\n"
            "frag_size=%u data_ptr=%u data_size=%u   alloc_err=%u logic_err=%u\n",
            pn->input ? queue_total_bytes(pn->input) : 0, queue_entry_count(pn->input),
            pn->output ? queue_total_bytes(pn->output) : 0, queue_entry_count(pn->output),
            (unsigned long long)pn->in_total_pkts, (unsigned long long)pn->in_total_bytes,
            (unsigned long long)pn->out_total_pkts, (unsigned long long)pn->out_total_bytes,
            pn->frag_size, pn->data_ptr, pn->data_size, pn->alloc_errors, pn->logic_errors) < 0) goto failed;
    }

    /* Links */
    int link_idx = 0;
    struct ETCP_LINK* link = conn->links;
    while (link) {
        const char* is_tcp_str = link->is_tcp ? "TCP" : "UDP";
        ip_str_t local = {.str = "stcp"}, remote = sockaddr_storage_to_str(&link->remote_addr);
        if (link->conn) local = sockaddr_storage_to_str(link->conn->interface_addr.ss_family ?
                                                      &link->conn->interface_addr : &link->conn->local_addr);
        if (strbuf_addf(&text,
            "\n--- LINK#%d: %s %s NAT=%s ---\n"
            "addr: %s -> %s\n"
            "RTT: %.1f ms   TT: %.1f ms\n"
            "Keepalive: sent=%u recv=%u  period=%ums\n"
            "Inflight: %u / %u B (%u pkts)\n"
            "Encrypt: sent=%zu err=%zu  Decrypt: rcvd=%zu err=%zu\n",
            link_idx, link->link_status ? "UP" : "DOWN", is_tcp_str, link_address_profile_text(link).text,
            local.str, remote.str,
            link->rtt_last / 10.0f, link->tt_last / 10.0f,
            link->keepalive_sent_count, link->keepalive_recv_count, link->keepalive_interval,
            link->inflight_bytes, link->inflight_lim_bytes, link->inflight_packets,
            link->total_encrypted, link->encrypt_errors, link->total_decrypted, link->decrypt_errors) < 0) goto failed;

        link = link->next; link_idx++;
    }

post:
    if (text.len > INT_MAX) goto failed;
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT, "connection metrics: peer=%016llX bytes=%zu", (unsigned long long)peer_node_id, text.len);
    chat_event_post(inst, CHAT_EVT_CONN_METRICS, (const uint8_t*)strbuf_str(&text), (int)text.len);
    strbuf_free(&text);
    return;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "connection metrics: peer=%016llX formatting/allocation failed after %zu bytes",
                (unsigned long long)peer_node_id, text.len);
    strbuf_free(&text);
}

void chat_core_collect_status_trampoline(void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    chat_core_collect_status(inst);
}

/* Снимок снимается в потоке ядра; GUI получает только готовый процент. */
void chat_core_collect_cpu_load_trampoline(void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    if (!inst || !inst->ua) { DEBUG_WARN(DEBUG_CATEGORY_SYS, "CPU load request: core unavailable"); return; }
    struct uasync_load load;
    uint8_t data[2] = {0, 0};
    if (uasync_get_load(inst->ua, &load) == ERR_OK && load.valid) {
        data[0] = 1; data[1] = (uint8_t)(load.average_percent + 0.5);
    }
    DEBUG_TRACE(DEBUG_CATEGORY_SYS, "CPU load GUI response: valid=%u average=%u%%", data[0], data[1]);
    chat_event_post(inst, CHAT_EVT_CPU_LOAD, data, sizeof(data));
}

void chat_core_collect_conn_list_trampoline(void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    collect_conn_list(inst);
}

/* Два прохода без callbacks: размер и данные относятся к одному состоянию uasync. */
void chat_core_collect_network_stats_trampoline(void* arg) {
    struct chat_network_stats_request* request = arg;
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "network stats: missing request"); return; }
    struct UTUN_INSTANCE* inst = request->inst;
    struct chat_network_stats_header header = {0};
    header.request_id = request->request_id; header.at_ms = get_time_us() / 1000;
    size_t size = sizeof(header);
    uint8_t* buffer = NULL;
    if (!inst || !request->request_id || !inst->connections) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "network stats: missing core/connections/request ID"); goto failed;
    }
    for (struct ll_entry* e = inst->connections->head; e; e = e->next) {
        struct conn_queue_entry* entry = (struct conn_queue_entry*)e->data;
        struct ETCP_CONN* conn = entry ? entry->conn : NULL;
        if (!conn || conn->state == 2) continue;
        size += sizeof(struct chat_network_stats_conn); header.count++;
        for (struct ETCP_LINK* link = conn->links; link; link = link->next)
            if (!link->closing) size += sizeof(struct chat_network_stats_link);
        if (size > INT_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "network stats: snapshot too large bytes=%zu", size); goto failed; }
    }
    buffer = u_calloc(1, size);
    if (!buffer) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "network stats: allocation failed bytes=%zu", size); goto failed; }
    uint8_t* p = buffer + sizeof(header);
    for (struct ll_entry* e = inst->connections->head; e; e = e->next) {
        struct conn_queue_entry* entry = (struct conn_queue_entry*)e->data;
        struct ETCP_CONN* conn = entry ? entry->conn : NULL;
        if (!conn || conn->state == 2) continue;
        struct chat_network_stats_conn value = {0};
        value.id = conn->diagnostic_id; value.peer_id = conn->peer_node_id; value.up = !!conn->links_up;
        value.tx_bytes = conn->diagnostic_wire_tx_bytes; value.rx_bytes = conn->diagnostic_wire_rx_bytes;
        if (value.peer_id && CC(inst) && CC(inst)->initialized)
            chat_core_get_node_name(inst, conn->crypto_ctx.peer_public_key, value.name, sizeof(value.name));
        if (!value.name[0]) snprintf(value.name, sizeof(value.name), "%s", conn->name ? conn->name : "ETCP");
        uint8_t* conn_record = p; p += sizeof(value);
        for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
            if (link->closing) continue;
            struct chat_network_stats_link item = {0};
            item.id = link->diagnostic_id; item.tx_bytes = link->diagnostic_wire_tx_bytes;
            item.rx_bytes = link->diagnostic_wire_rx_bytes; item.inflight = link->inflight_bytes;
            if (etcp_link_get_snapshot(link, &item.description)) goto failed;
            memcpy(p, &item, sizeof(item)); p += sizeof(item); value.link_count++;
        }
        memcpy(conn_record, &value, sizeof(value));
    }
    memcpy(buffer, &header, sizeof(header));
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT, "network stats: request=%llu connections=%u bytes=%zu at_ms=%llu",
        (unsigned long long)header.request_id, header.count, size, (unsigned long long)header.at_ms);
    chat_event_post(inst, CHAT_EVT_NETWORK_STATS, buffer, (int)size);
    u_free(buffer); u_free(request);
    return;
failed:
    u_free(buffer); header.count = 0; header.failed = 1;
    if (inst) chat_event_post(inst, CHAT_EVT_NETWORK_STATS, (const uint8_t*)&header, sizeof(header));
    u_free(request);
}

void chat_core_collect_conn_metrics_trampoline(void* arg) {
    struct chat_node_arg* a = (struct chat_node_arg*)arg;
    if (!a) return;
    uint64_t peer_id = a->node_id;
    collect_conn_metrics(a->inst, peer_id);
    u_free(arg);
}

/* Наличие записи читается worker, live-диагностика собирается в uasync; wire описан в chat_member.h. */
#if CHAT_MEMBER_DETAIL_MAX_HOPS != MAX_HOPS
#error "member detail hop limit must match routing"
#endif
#if CHAT_MEMBER_DETAIL_LINK_SIZE != 42 + 20 + ETCP_LINK_PROTOCOL_SIZE
#error "member detail link size must match the serialized snapshot"
#endif

/* Проверяем размеры, адресата и все строки до изменения состояния GUI. */
int chat_core_parse_member_detail(const uint8_t* data, size_t len, struct chat_member_detail_view* out) {
    if (!data || !out || len < CHAT_MEMBER_DETAIL_HEADER_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail: missing header bytes=%zu", len); return -1;
    }
    struct chat_member_detail_view v = {0};
    v.request_id = mf_get64(data); memcpy(v.group, data + 8, 32); memcpy(v.ed, data + 40, 32);
    v.result = data[CHAT_MEMBER_DETAIL_RESULT_OFF]; v.transport_flags = data[CHAT_MEMBER_DETAIL_RESULT_OFF + 1];
    v.own_tcp_active = (uint16_t)data[CHAT_MEMBER_DETAIL_RESULT_OFF + 2] | ((uint16_t)data[CHAT_MEMBER_DETAIL_RESULT_OFF + 3] << 8);
    v.hop_count = data[CHAT_MEMBER_DETAIL_HOP_COUNT_OFF]; v.link_count = data[CHAT_MEMBER_DETAIL_LINK_COUNT_OFF]; v.sock_count = data[CHAT_MEMBER_DETAIL_SOCK_COUNT_OFF];
    size_t expected = CHAT_MEMBER_DETAIL_HEADER_SIZE + (size_t)v.hop_count * 32 +
        (size_t)v.link_count * CHAT_MEMBER_DETAIL_LINK_SIZE + (size_t)v.sock_count * CHAT_MEMBER_DETAIL_SOCK_SIZE;
    if (((!v.request_id || mf_zero(v.group, 32) || mf_zero(v.ed, 32)) && v.result != CHAT_MEMBER_DETAIL_INVALID) ||
        v.result > CHAT_MEMBER_DETAIL_ERROR || v.transport_flags > 7 ||
        v.hop_count > CHAT_MEMBER_DETAIL_MAX_HOPS || v.link_count > CHAT_MEMBER_DETAIL_MAX_LINKS ||
        v.sock_count > CHAT_MEMBER_DETAIL_MAX_SOCKS || expected != len ||
        (v.result && (v.transport_flags || v.own_tcp_active || v.hop_count || v.link_count || v.sock_count))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail: invalid packet request=%llu group=%s peer=%s bytes=%zu expected=%zu",
            (unsigned long long)v.request_id, sc_public_key_hex(v.group, (char[65]){0}), sc_public_key_hex(v.ed, (char[65]){0}), len, expected);
        return -1;
    }
    v.hops = data + CHAT_MEMBER_DETAIL_HEADER_SIZE; v.links = v.hops + v.hop_count * 32;
    v.socks = v.links + v.link_count * CHAT_MEMBER_DETAIL_LINK_SIZE;
    for (int i = 0; i < v.hop_count; i++) if (mf_zero(v.hops + i * 32, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail: zero hop index=%d", i); return -1;
    }
    for (int i = 0; i < v.link_count; i++) {
        const uint8_t* link = v.links + i * CHAT_MEMBER_DETAIL_LINK_SIZE;
        if (!memchr(link + 42, 0, 20) || !link[62] || !memchr(link + 62, 0, 32) ||
            link[0] > PACKET_LINK_INCOMING || link[3] > 1 ||
            (link[4] && link[4] != 4 && link[4] != 6) || (link[23] && link[23] != 4 && link[23] != 6) ||
            (!link[3] && (link[4] || link[21] || link[22] || link[23] || link[40] || link[41]))) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail: invalid link index=%d", i); return -1;
        }
    }
    for (int i = 0; i < v.sock_count; i++) if (!memchr(v.socks + i * CHAT_MEMBER_DETAIL_SOCK_SIZE + 10, 0, 38)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail: unterminated interface index=%d", i); return -1;
    }
    *out = v;
    return 0;
}

/* Повторяет Qt: name.lastIndexOf("_v") → name.mid(3, idx - 3) */
static void sock_ifname(const char* name, char* out, size_t out_sz) {
    out[0] = '\0';
    if (!name || !name[0]) return;
    const char* last = NULL;
    for (const char* q = name; (q = strstr(q, "_v")) != NULL; q++) last = q;
    if (!last) return;
    int idx = (int)(last - name);
    if (idx <= 3) return;
    int len = idx - 3;
    if (len >= (int)out_sz) len = (int)out_sz - 1;
    memcpy(out, name + 3, len);
    out[len] = '\0';
}

/* Состояние runtime проверяется до SQL и повторно при доставке, без обращения к БД. */
static int member_detail_state(const struct chat_member_detail_req* req, struct TOPO_GROUP** out) {
    struct UTUN_INSTANCE* inst = req->inst;
    *out = inst && inst->topo_groups ? topo_groups_find_wire(inst->topo_groups, req->group) : NULL;
    if (!req->request_id || mf_zero(req->group, 32) || mf_zero(req->ed, 32)) return CHAT_MEMBER_DETAIL_INVALID;
    if (!inst || !CC(inst) || !CC(inst)->initialized || !*out || (*out)->stopping || (*out)->group_type != TOPO_GROUP_TYPE_CHAT)
        return CHAT_MEMBER_DETAIL_UNAVAILABLE;
    if (!chat_channel_valid((*out)->channel_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail: invalid stored channel group=%s channel=%s",
            sc_public_key_hex(req->group, (char[65]){0}), (*out)->channel_id); return CHAT_MEMBER_DETAIL_ERROR;
    }
    return CHAT_MEMBER_DETAIL_OK;
}

static void collect_member_detail(const struct chat_member_detail_req* req, int result) {
    struct UTUN_INSTANCE* inst = req->inst;
    uint8_t buf[CHAT_MEMBER_DETAIL_MAX_SIZE] = {0};
    mf_put64(buf, req->request_id); memcpy(buf + 8, req->group, 32); memcpy(buf + 40, req->ed, 32);
    struct TOPO_GROUP* group = NULL; int state = member_detail_state(req, &group);
    buf[CHAT_MEMBER_DETAIL_RESULT_OFF] = (uint8_t)(state ? state : result);
    if (buf[CHAT_MEMBER_DETAIL_RESULT_OFF]) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member detail: rejected request=%llu group=%s peer=%s result=%u",
            (unsigned long long)req->request_id, sc_public_key_hex(req->group, (char[65]){0}), sc_public_key_hex(req->ed, (char[65]){0}), buf[CHAT_MEMBER_DETAIL_RESULT_OFF]);
        chat_event_post(inst, CHAT_EVT_MEMBER_DETAIL, buf, CHAT_MEMBER_DETAIL_HEADER_SIZE); return;
    }

    uint8_t flags = 0;
    struct ETCP_CONN* conn = instance_find_conn_by_ed(inst, req->ed);
    if (conn) {
        flags |= 1;
        if (conn->links_up) flags |= 2;
        if (conn->initialized) flags |= 4;
    }
    uint32_t own_tcp_active = 0;
    if (!memcmp(req->ed, inst->my_ed25519_pubkey, 32)) {
        if (inst->connections) {
            for (struct ll_entry* e = inst->connections->head; e; e = e->next) {
                struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
                if (ce && ce->conn && ce->conn->initialized) {
                    for (struct ETCP_LINK* lk = ce->conn->links; lk; lk = lk->next)
                        if (lk->is_tcp) { own_tcp_active++; break; }
                }
            }
        }
    }

    uint8_t link_count = 0;
    if (own_tcp_active > UINT16_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member detail: active TCP count exceeds wire limit=%u", own_tcp_active);
        own_tcp_active = UINT16_MAX;
    }
    for (struct ETCP_LINK* lk = conn ? conn->links : NULL; lk && link_count < CHAT_MEMBER_DETAIL_MAX_LINKS; lk = lk->next)
        link_count++;

    uint8_t sock_count = 0;
    for (struct ETCP_SOCKET* s = inst->etcp_sockets; s && sock_count < CHAT_MEMBER_DETAIL_MAX_SOCKS; s = s->next)
        sock_count++;

    struct TOPO_GROUP_NODE* node = topo_node_find_by_ed(group, req->ed);
    uint8_t hop_count = 0;
    const struct topo_hop* hops = topo_node_best_hop_list(node, &hop_count, NULL);
    buf[CHAT_MEMBER_DETAIL_RESULT_OFF + 1] = flags; buf[CHAT_MEMBER_DETAIL_RESULT_OFF + 2] = (uint8_t)own_tcp_active; buf[CHAT_MEMBER_DETAIL_RESULT_OFF + 3] = (uint8_t)(own_tcp_active >> 8);
    buf[CHAT_MEMBER_DETAIL_HOP_COUNT_OFF] = hop_count; buf[CHAT_MEMBER_DETAIL_LINK_COUNT_OFF] = link_count; buf[CHAT_MEMBER_DETAIL_SOCK_COUNT_OFF] = sock_count;
    uint8_t* p = buf + CHAT_MEMBER_DETAIL_HEADER_SIZE;
    for (int i = 0; i < hop_count; i++, p += 32) memcpy(p, hops[i].ed, 32);

    for (struct ETCP_LINK* lk = conn ? conn->links : NULL; lk && link_count; lk = lk->next, link_count--) {
        struct etcp_link_snapshot snapshot;
        if (etcp_link_get_snapshot(lk, &snapshot)) {
            buf[CHAT_MEMBER_DETAIL_RESULT_OFF] = CHAT_MEMBER_DETAIL_ERROR;
            memset(buf + 49, 0, CHAT_MEMBER_DETAIL_HEADER_SIZE - 49);
            chat_event_post(inst, CHAT_EVT_MEMBER_DETAIL, buf, CHAT_MEMBER_DETAIL_HEADER_SIZE); return;
        }
        *p++ = snapshot.direction; *p++ = snapshot.state; *p++ = snapshot.status; *p++ = snapshot.has_ip;
        *p++ = snapshot.local.family; memcpy(p, snapshot.local.bytes, 16); p += 16;
        *p++ = (uint8_t)snapshot.local.port; *p++ = (uint8_t)(snapshot.local.port >> 8);
        *p++ = snapshot.remote.family; memcpy(p, snapshot.remote.bytes, 16); p += 16;
        *p++ = (uint8_t)snapshot.remote.port; *p++ = (uint8_t)(snapshot.remote.port >> 8);
        memcpy(p, snapshot.socket_name, 20); p[19] = 0; p += 20;
        memcpy(p, snapshot.protocol, sizeof(snapshot.protocol)); p += sizeof(snapshot.protocol);
    }

    for (struct ETCP_SOCKET* s = inst->etcp_sockets; s && sock_count; s = s->next, sock_count--) {
        uint32_t sid = s->sock_id;
        uint16_t sp = 0;
        const struct sockaddr_storage* sa = s->interface_addr.ss_family ? &s->interface_addr : &s->local_addr;
        if (sa->ss_family == AF_INET) sp = ntohs(((const struct sockaddr_in*)sa)->sin_port);
        else if (sa->ss_family == AF_INET6) sp = ntohs(((const struct sockaddr_in6*)sa)->sin6_port);
        uint32_t lu = 0, ld = 0;
        {   struct ll_entry* e = inst->connections ? inst->connections->head : NULL;
            for (; e; e = e->next) {
                struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
                if (!ce || !ce->conn) continue;
                for (struct ETCP_LINK* lk = ce->conn->links; lk; lk = lk->next)
                    if (lk->conn == s) { if (lk->link_state == 3) lu++; else ld++; }
            }
        }
        char ifname[38] = {0}; sock_ifname(s->name, ifname, sizeof(ifname));
        if (lu > UINT16_MAX || ld > UINT16_MAX) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member detail: socket link counts exceed wire limit id=%u up=%u down=%u", sid, lu, ld);
            if (lu > UINT16_MAX) lu = UINT16_MAX;
            if (ld > UINT16_MAX) ld = UINT16_MAX;
        }
        for (int i = 0; i < 4; i++) *p++ = (uint8_t)(sid >> (8 * i));
        *p++ = (uint8_t)sp; *p++ = (uint8_t)(sp >> 8);
        *p++ = (uint8_t)lu; *p++ = (uint8_t)(lu >> 8);
        *p++ = (uint8_t)ld; *p++ = (uint8_t)(ld >> 8);
        memcpy(p, ifname, 38); p += 38;
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member detail: request=%llu group=%s peer=%s hops=%u links=%u socks=%u bytes=%zu",
        (unsigned long long)req->request_id, sc_public_key_hex(req->group, (char[65]){0}), sc_public_key_hex(req->ed, (char[65]){0}),
        buf[CHAT_MEMBER_DETAIL_HOP_COUNT_OFF], buf[CHAT_MEMBER_DETAIL_LINK_COUNT_OFF], buf[CHAT_MEMBER_DETAIL_SOCK_COUNT_OFF], (size_t)(p - buf));
    chat_event_post(inst, CHAT_EVT_MEMBER_DETAIL, buf, (int)(p - buf));
}

struct member_detail_query { char channel[65]; uint8_t ed[32]; int found; };
struct member_detail_job {
    struct member_detail_query sql; /* Первое поле — только frozen SQL-данные, доступные worker. */
    struct chat_member_detail_req* request;
    sqlite3* expected;
    uint64_t generation;
};

static int member_detail_read(sqlite3* db, void* arg) {
    struct member_detail_query* query = arg;
    return chat_member_exists_read(db, query->channel, query->ed, &query->found);
}

static void member_detail_done(void* arg, const struct db_async_result* result) {
    struct member_detail_job* job = arg; struct chat_member_detail_req* req = job->request;
    struct UTUN_INSTANCE* inst = req->inst; struct chat_core_ctx* cc = CC(inst);
    int current = cc && cc->initialized && cc->db == job->expected && inst->group_generation == job->generation;
    int status = result->status ? CHAT_MEMBER_DETAIL_ERROR : job->sql.found ? CHAT_MEMBER_DETAIL_OK : CHAT_MEMBER_DETAIL_NOT_MEMBER;
    if (!current) status = CHAT_MEMBER_DETAIL_UNAVAILABLE;
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail READ failed request=%llu ch=%s peer=%s status=%d rc=%d: %s",
        (unsigned long long)req->request_id, job->sql.channel, sc_public_key_hex(req->ed, (char[65]){0}),
        result->status, result->sqlite_code, result->message);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member detail READ result request=%llu ch=%s peer=%s found=%d current=%d result=%d",
        (unsigned long long)req->request_id, job->sql.channel, sc_public_key_hex(req->ed, (char[65]){0}), job->sql.found, current, status);
    u_free(job); collect_member_detail(req, status); u_free(req);
}

void chat_core_collect_member_detail_trampoline(void* arg) {
    struct chat_member_detail_req* req = arg;
    if (!req || !req->inst) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail: missing request/instance"); u_free(req); return; }
    struct TOPO_GROUP* group = NULL; int state = member_detail_state(req, &group);
    if (state) { collect_member_detail(req, state); u_free(req); return; }
    struct UTUN_INSTANCE* inst = req->inst;
    struct member_detail_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member detail request allocation failed request=%llu group=%016llx peer=%s",
            (unsigned long long)req->request_id, sc_public_key_hex(req->group, (char[65]){0}), sc_public_key_hex(req->ed, (char[65]){0}));
        collect_member_detail(req, CHAT_MEMBER_DETAIL_ERROR); u_free(req); return;
    }
    job->request = req; job->expected = CC(inst)->db; job->generation = inst->group_generation; memcpy(job->sql.ed, req->ed, 32);
    strcpy(job->sql.channel, group->channel_id);
    if (inst->db_worker) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member detail READ queued request=%llu ch=%s peer=%s",
            (unsigned long long)req->request_id, job->sql.channel, sc_public_key_hex(req->ed, (char[65]){0}));
        db_async_submit(inst->db_worker, "member-detail", DB_ASYNC_READ, member_detail_read, job, member_detail_done); return;
    }
    const char* file = job->expected ? sqlite3_db_filename(job->expected, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_MISUSE};
    if (file && !*file) {
        result.sqlite_code = member_detail_read(job->expected, job);
        result.status = result.sqlite_code == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL;
        snprintf(result.message, sizeof(result.message), "%s", sqlite3_errstr(result.sqlite_code));
    } else snprintf(result.message, sizeof(result.message), "file database has no asynchronous reader");
    member_detail_done(job, &result);
}
