#include "etcp_dump.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "pkt_normalizer.h"
#include "../../lib/ll_queue.h"
#include "../../lib/memory_pool.h"
#include "../../lib/socket_compat.h"
#include "../../lib/debug_config.h"
#include <stdio.h>
#include <string.h>

#define DLOG(fmt, ...) DEBUG_DEBUG(DEBUG_CATEGORY_ETCP_DUMP, "  " fmt, ##__VA_ARGS__)
#define DHDR(fmt, ...) DEBUG_DEBUG(DEBUG_CATEGORY_ETCP_DUMP, fmt, ##__VA_ARGS__)

static void dump_queue(const char* name, struct ll_queue* q) {
    if (!q) { DLOG("%-14s NULL", name); return; }
    int suspended = 0;
#ifdef __cplusplus
    suspended = q->callback_suspended ? 1 : 0;
#else
    suspended = q->callback_suspended ? 1 : 0;
#endif
    DLOG("%-14s %d pkts / %zu bytes  suspended=%d", name,
         queue_entry_count(q), queue_total_bytes(q), suspended);
}

static void dump_pool(const char* name, struct memory_pool* pool) {
    if (!pool) return;
    size_t allocs, reuse;
    memory_pool_get_stats(pool, &allocs, &reuse);
    DLOG("  %-12s alloc=%zu reuse=%zu", name, allocs, reuse);
}

static const char* ip_to_str_buf(uint32_t ip, char* buf, size_t sz) {
    uint8_t* b = (uint8_t*)&ip;
    snprintf(buf, sz, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

static const char* sockaddr_to_str(const struct sockaddr_storage* addr, char* buf, size_t sz) {
    if (!addr || addr->ss_family != AF_INET) { snprintf(buf, sz, "?"); return buf; }
    const struct sockaddr_in* sin = (const struct sockaddr_in*)addr;
    uint8_t* b = (uint8_t*)&sin->sin_addr.s_addr;
    snprintf(buf, sz, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(sin->sin_port));
    return buf;
}

static const char* link_state_str(int s) {
    switch (s) { case 0: return "IDLE"; case 1: return "INIT"; case 2: return "RECONNECT"; case 3: return "CONNECTED"; default: return "?"; }
}

void etcp_dump_conn_state(struct ETCP_CONN* conn) {
    if (!conn) { DHDR("=== ETCP DUMP: NULL conn ==="); return; }

    char addr_buf[32], nat_buf[32];

    DHDR("=== ETCP CONN [%s] ===", conn->log_name);

    DLOG("GENERAL: peer=0x%016llx state=%d init=%d l_up=%d tx=%d mtu=%d",
         (unsigned long long)conn->peer_node_id, conn->state, conn->initialized, conn->links_up,
         conn->tx_state, conn->mtu);

    DLOG("IDS: next_tx=%u last_rx=%u last_del=%u rx_ack_till=%u got_init=%d reset_done=%d",
         conn->next_tx_id, conn->last_rx_id, conn->last_delivered_id, conn->rx_ack_till,
         conn->got_initial_pkt, conn->reset_done);

    DLOG("RTT: last=%u avg10=%u jitter=%u",
         conn->rtt_last, conn->rtt_avg_10, conn->jitter);

    DLOG("STATS: bytes_sent=%u retrans=%u reinit=%u reset=%u acks=%u dup_rx=%u dup_tx=%u",
         conn->bytes_sent_total, conn->retransmissions_count,
         conn->reinit_count, conn->reset_count, conn->ack_packets_count,
         conn->rx_dup_count, conn->tx_dup_count);

    DLOG("INFLIGHT: unacked=%u optimal=%u max=%u",
         conn->unacked_bytes, conn->optimal_inflight, conn->max_inflight);

    DLOG("ACK: hit_inf=%u hit_sndq=%u miss=%u link_wait=%u",
         conn->cnt_ack_hit_inf, conn->cnt_ack_hit_sndq, conn->cnt_ack_miss, conn->cnt_link_wait);

    DLOG("TIMERS: ack_resp=%s peer_window=%s",
         conn->ack_resp_timer ? "ACTIVE" : "free", conn->ack_window_timer ? "ACTIVE" : "free");

    DHDR("  --- QUEUES ---");
    dump_queue("input",        conn->input_queue);
    dump_queue("send_input_q", conn->send_input_q);
    dump_queue("input_send_q", conn->input_send_q);
    dump_queue("input_wait_ack", conn->input_wait_ack);
    dump_queue("recv_q",       conn->recv_q);
    dump_queue("output",       conn->output_queue);
    if (conn->transit_queues)
        DLOG("%-14s %d entries", "transit", queue_entry_count(conn->transit_queues));

    if (conn->normalizer) {
        struct PKTNORM* pn = (struct PKTNORM*)conn->normalizer;
        DHDR("  --- NORMALIZER ---");
        DLOG("frag=%u data_ptr=%u/%u flush=%s recvpart=%s",
             pn->frag_size, pn->data_ptr, pn->data_size,
             pn->flush_timer ? "ACTIVE" : "free", pn->recvpart ? "ACTIVE" : "free");
        dump_queue("pn-input",  pn->input);
        dump_queue("pn-output", pn->output);
        DLOG("err: alloc=%u logic=%u in=%llu/%llu out=%llu/%llu",
             pn->alloc_errors, pn->logic_errors,
             (unsigned long long)pn->in_total_pkts, (unsigned long long)pn->in_total_bytes,
             (unsigned long long)pn->out_total_pkts, (unsigned long long)pn->out_total_bytes);
    } else {
        DHDR("  --- NORMALIZER --- NULL");
    }

    DHDR("  --- POOLS ---");
    dump_pool("inflight", conn->inflight_pool);
    dump_pool("attempts", conn->attempt_pool);
    dump_pool("io",       conn->io_pool);
    if (conn->instance) {
        dump_pool("data",  conn->instance->data_pool);
        dump_pool("ack",   conn->instance->ack_pool);
        dump_pool("pkt",   conn->instance->pkt_pool);
    }

    int link_idx = 0;
    struct ETCP_LINK* link = conn->links;
    while (link) {
        DHDR("  --- LINK %d ---", link_idx);
        sockaddr_to_str(&link->remote_addr, addr_buf, sizeof(addr_buf));
        DLOG("STATE: id=%d/%d sv=%d st=%d(%s) status=%d init=%d mtu=%d/%d",
             link->local_link_id, link->remote_link_id, link->is_server,
             link->link_state, link_state_str(link->link_state),
             link->link_status, link->initialized, link->mtu, link->mtu_local);
        DLOG("ADDR: remote=%s sock=%d type=%d only_local=%d mtu_remote=%d",
             addr_buf, link->remote_socket_id, link->remote_type, link->remote_only_local, link->mtu_remote);
        DLOG("TIMERS: init=%s(to=%u/retry=%u) keepalive=%s ka_timer=%s shaper=%s burst=%s",
             link->init_timer ? "ACTIVE" : "free", link->init_timeout, link->init_retry_count,
             link->keepalive_sent_count > 0 ? "ACTIVE" : "idle",
             link->keepalive_timer ? "ACTIVE" : "free",
             link->shaper_timer ? "ACTIVE" : "free",
             link->burst_resp_timer ? "ACTIVE" : "free");
        DLOG("KA: recv=%d remote=%d sent=%u recv=%u ival=%u to=%u",
             link->recv_keepalive, link->remote_keepalive,
             link->keepalive_sent_count, link->keepalive_recv_count,
             link->keepalive_interval, link->keepalive_timeout);
        DLOG("INFLIGHT: bytes=%u pkts=%u lim=%u pacing=%u",
             link->inflight_bytes, link->inflight_packets,
             link->inflight_lim_bytes, link->bbr_pacing_rate);
        dump_queue("attempts", link->attempts); dump_queue("path_ACK", link->ack_q);
        DLOG("RTO: timer=%s timeout_us=%u DATA_RTT_us=%u jitter_us=%u",
            link->retrans_timer ? "ACTIVE" : "free", etcp_tx_rto_us(link), link->data_rtt_us, link->data_jitter_us);
        DLOG("RTT: last=%u jitter=%u min_rtt_us=%u",
             link->rtt_last, link->jitter,
             link->bbr ? link->bbr->min_rtt_us : 0);
        DLOG("TT/RT: tt=%u rt=%u dt_tx=%u dt_rx=%u bw=%u",
             link->tt_last, link->rt_last,
             link->recv_dt_avg_tx, link->recv_dt_avg_rx, link->bandwidth);
        const struct ETCP_ACK_TIMING* timing = &link->ack_timing;
        DLOG("ACK_TIMING: attempt=%llu local_send=%llu peer_receive=%llu peer_ACK=%llu local_ACK=%llu clock_fwd=%lld clock_back=%lld",
             (unsigned long long)timing->attempt_id, (unsigned long long)timing->local_send_us,
             (unsigned long long)timing->peer_receive_us, (unsigned long long)timing->peer_ack_us,
             (unsigned long long)timing->local_ack_us, (long long)timing->forward_clock_us, (long long)timing->backward_clock_us);
        DLOG("ERRORS: enc=%zu dec=%zu snd=%zu rcv=%zu total_enc=%zu total_dec=%zu retrans=%u",
             link->encrypt_errors, link->decrypt_errors,
             link->send_errors, link->recv_errors,
             link->total_encrypted, link->total_decrypted, link->total_retransmissions);
        DLOG("OWN_ENDPOINT_OBSERVED: addr=%s changes=%u hits=%u",
             sockaddr_storage_to_str(&link->local_as_seen_by_peer).str,
             link->nat_changes_count, link->nat_hits_count);
        DLOG("LAST_RECV: tb=%llu ts=%u",
             (unsigned long long)link->last_recv_local_time, link->last_recv_timestamp);
        DLOG("BURST: active=%d id=%u seq=%u/%u",
             link->burst_active, link->burst_id, link->burst_seq, link->burst_count);
        link = link->next;
        link_idx++;
    }
    if (link_idx == 0) DHDR("  --- NO LINKS ---");

    DHDR("=== END [%s] ===", conn->log_name);
}

void etcp_dump_all_conns(struct UTUN_INSTANCE* instance) {
    if (!instance) { DHDR("=== ETCP DUMP: NULL instance ==="); return; }
    DHDR("=== ETCP DUMP ALL CONNS node=0x%016llx ===", (unsigned long long)instance->node_id);
    struct ll_entry* entry = instance->connections ? instance->connections->head : NULL;
    int idx = 0;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        if (ce && ce->conn) { etcp_dump_conn_state(ce->conn); idx++; }
        entry = entry->next;
    }
    if (idx == 0) DHDR("--- NO CONNECTIONS ---");
    DHDR("=== END DUMP ALL CONNS ===");
}

void etcp_dump_sockets(struct UTUN_INSTANCE* instance) {
    if (!instance) { DHDR("=== ETCP DUMP: NULL instance ==="); return; }
    DHDR("=== ETCP DUMP SOCKETS node=0x%016llx ===", (unsigned long long)instance->node_id);
    struct ETCP_SOCKET* s = instance->etcp_sockets;
    int idx = 0;
    while (s) {
        char abuf[32]; sockaddr_to_str(&s->local_addr, abuf, sizeof(abuf));
        DLOG("SOCK %d: '%s' fd=%d addr=%s config=%u mapping=%s inbound=%s mtu=%d links=%d",
             idx, s->name, (int)s->fd, abuf, s->type, address_mapping_name(s->address_profile.mapping),
             address_inbound_name(s->address_profile.inbound),
             s->mtu, s->links_queue ? queue_entry_count(s->links_queue) : 0);
        s = s->next; idx++;
    }
    if (idx == 0) DHDR("--- NO SOCKETS ---");
    if (instance->stcp_servers) DLOG("TCP: stcp_servers count=%d", stcp_server_list_count(instance));
    DHDR("=== END DUMP SOCKETS ===");
}

void etcp_dump_all(struct UTUN_INSTANCE* instance) {
    if (!instance) { DHDR("=== ETCP DUMP: NULL instance ==="); return; }
    DHDR("=== ETCP FULL DUMP node=0x%016llx ===", (unsigned long long)instance->node_id);
    etcp_dump_sockets(instance);
    etcp_dump_all_conns(instance);
    DHDR("=== END FULL DUMP ===");
}
