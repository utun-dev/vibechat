#include "ntp_node_time.h"
#include "ntp_time.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "etcp_api.h"
#include "etcp.h"
#include "etcp_connections.h"
#include <string.h>
#include <stdlib.h>

#define TIME_SYNC_FLAG_SYNCED 0x01
#define TIME_SYNC_FLAG_FRESH  0x02

#pragma pack(push, 1)
struct time_sync_msg {
    uint8_t  flags;         // bit0 = есть поправка; bit1 = синхронизация после запуска, не сохранённый кэш
    int64_t  t_sender_us;   // corrected time if synced, local gettimeofday otherwise
};
#pragma pack(pop)

_Static_assert(sizeof(struct time_sync_msg) == 9, "time_sync_msg size mismatch");

#define TIME_SYNC_PKT_SIZE (1 + sizeof(struct time_sync_msg))  // cmd(1) + msg(9) = 10

static void ntp_node_on_conn_init(struct ETCP_CONN* conn, int event, void* arg);
static void ntp_node_on_new_conn(struct ETCP_CONN* conn, void* arg);
static void ntp_node_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry);
static int ntp_node_subscribe(struct ETCP_CONN* conn, struct UTUN_INSTANCE* inst);

static int send_time_sync(struct UTUN_INSTANCE* inst, struct ETCP_CONN* conn) {
    if (!inst || !conn) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: TIME_SYNC requires instance and connection");
        return -1;
    }

    struct time_sync_msg msg;
    msg.flags = ntp_time_is_synced(inst) ? TIME_SYNC_FLAG_SYNCED : 0;
    if (inst->ntp.source >= NTP_TIME_PEER) msg.flags |= TIME_SYNC_FLAG_FRESH;

    int64_t now_us;
    if (ntp_time_is_synced(inst)) {
        now_us = ntp_time_get_us(inst);
    } else {
        struct timeval tv;
#ifdef _WIN32
        utun_gettimeofday(&tv, NULL);
#else
        gettimeofday(&tv, NULL);
#endif
        now_us = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
    }
    msg.t_sender_us = now_us;

    uint8_t* pkt = u_malloc(TIME_SYNC_PKT_SIZE);
    if (!pkt) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: u_malloc failed for TIME_SYNC"); return -1; }
    pkt[0] = ETCP_ID_NTP_TIME;
    memcpy(pkt + 1, &msg, sizeof(msg));

    struct ll_entry* e = queue_entry_new(0);
    if (!e) { u_free(pkt); DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: queue_entry_new failed for TIME_SYNC"); return -1; }
    e->dgram = pkt;
    e->len   = TIME_SYNC_PKT_SIZE;

    if (etcp_send(conn, e) != 0) {
        u_free(pkt); queue_entry_free(e);
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: etcp_send failed to node %s", sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}));
        return -1;
    }
    return 0;
}

static int64_t gettimeofday_us(void) {
    struct timeval tv;
#ifdef _WIN32
    utun_gettimeofday(&tv, NULL);
#else
    gettimeofday(&tv, NULL);
#endif
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static struct NTP_NODE_PEER* find_peer(struct NTP_NODE_TIME* np, const uint8_t peer_key[32]) {
    for (int i = 0; i < np->peer_count; i++) {
        if (!memcmp(np->peers[i].ed25519, peer_key, 32)) return &np->peers[i];
    }
    return NULL;
}

static struct NTP_NODE_PEER* add_peer(struct NTP_NODE_TIME* np, const uint8_t peer_key[32]) {
    int idx = np->peer_count;
    if (idx < NTP_NODE_MAX_PEERS) {
        np->peer_count++;
    } else {
        idx = 0;
        for (int i = 1; i < NTP_NODE_MAX_PEERS; i++) {
            if (np->peers[i].last_seen_tb < np->peers[idx].last_seen_tb) idx = i;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP_node: replacing diagnostic peer %s with %s",
                    sc_public_key_hex(np->peers[idx].ed25519, (char[65]){0}), sc_public_key_hex(peer_key, (char[65]){0}));
    }
    struct NTP_NODE_PEER* p = &np->peers[idx];
    memset(p, 0, sizeof(*p));
    memcpy(p->ed25519, peer_key, 32);
    return p;
}

static void check_drift(const uint8_t peer_key[32], int64_t offset_delta_us) {
    int64_t abs_us = offset_delta_us < 0 ? -offset_delta_us : offset_delta_us;
    if (abs_us > NTP_NODE_DRIFT_ERROR_US) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: clock offset changed by >10s with node %s: %lldus",
                    sc_public_key_hex(peer_key, (char[65]){0}), (long long)offset_delta_us);
    } else if (abs_us > NTP_NODE_DRIFT_WARN_US) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP_node: clock offset changed by >2s with node %s: %lldus",
                   sc_public_key_hex(peer_key, (char[65]){0}), (long long)offset_delta_us);
    }
}

static void ntp_node_on_conn_init(struct ETCP_CONN* conn, int event, void* arg) { (void)event;
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    if (!inst || !conn || !inst->ntp_node.active) return;

    if (send_time_sync(inst, conn) == 0) {
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP_node: sent TIME_SYNC to node %s (synced=%d)",
                   sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}), ntp_time_is_synced(inst));
    }
}

static void ntp_node_on_new_conn(struct ETCP_CONN* conn, void* arg) {
    if (!conn) return;
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    if (!inst || !inst->ntp_node.active) return;
    ntp_node_subscribe(conn, inst);
}

static int ntp_node_subscribe(struct ETCP_CONN* conn, struct UTUN_INSTANCE* inst) {
    // API возвращает void; отсутствие новой головы цепочки означает отказ выделения памяти.
    struct etcp_cbk_entry* prev = conn->cbks;
    etcp_conn_add_cbk(conn, ntp_node_on_conn_init, inst, ETCP_CBK_EVENT_INIT);
    if (conn->cbks == prev) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: cannot subscribe connection %p node=%s",
                    (void*)conn, sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}));
        return -1;
    }
    return 0;
}

static void ntp_node_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!conn || !entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: receive requires connection and entry");
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }
    struct UTUN_INSTANCE* inst = conn->instance;
    if (!inst || !inst->ntp_node.active) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP_node: received TIME_SYNC without active module");
        queue_dgram_free(entry); queue_entry_free(entry);
        return;
    }
    if (!entry->dgram || entry->len != TIME_SYNC_PKT_SIZE || entry->dgram[0] != ETCP_ID_NTP_TIME) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP_node: invalid TIME_SYNC node=%s bytes=%d",
                    sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}), entry->len);
        queue_dgram_free(entry); queue_entry_free(entry);
        return;
    }

    if (!memcmp(conn->peer_ed25519_pubkey, (uint8_t[32]){0}, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP_node: TIME_SYNC from transport without a peer key");
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }

    struct time_sync_msg* msg = (struct time_sync_msg*)(entry->dgram + 1);
    int sender_synced = (msg->flags & TIME_SYNC_FLAG_SYNCED) != 0;
    int sender_fresh = (msg->flags & TIME_SYNC_FLAG_FRESH) != 0;
    if ((msg->flags & ~(TIME_SYNC_FLAG_SYNCED | TIME_SYNC_FLAG_FRESH)) || (sender_fresh && !sender_synced)
        || msg->t_sender_us <= 0 || msg->t_sender_us > NTP_TIME_LIMIT_US) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP_node: invalid time metadata node=%s flags=%u time=%lld",
            sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}), msg->flags, (long long)msg->t_sender_us);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }

    int64_t t4 = gettimeofday_us();
    if (t4 <= 0 || t4 > NTP_TIME_LIMIT_US) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP_node: local clock outside supported range time=%lld", (long long)t4);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    int64_t peer_offset = t4 - msg->t_sender_us;

    struct NTP_NODE_TIME* np = &inst->ntp_node;
    struct NTP_NODE_PEER* peer = find_peer(np, conn->peer_ed25519_pubkey);
    if (!peer) peer = add_peer(np, conn->peer_ed25519_pubkey);

    if (peer->has_offset) {
        int64_t offset_delta = peer_offset - peer->offset_us;
        check_drift(conn->peer_ed25519_pubkey, offset_delta);
    }
    peer->offset_us = peer_offset;
    peer->has_offset = 1;
    peer->last_seen_tb = get_time_tb();

    enum ntp_time_source previous = inst->ntp.source;

    if (sender_synced) {
        int64_t rtt_us = 0, n = 0;
        for (struct ETCP_LINK* l = conn->links; l; l = l->next) { rtt_us += l->rtt_last * 100LL; n++; }
        int64_t offset = peer_offset - (n ? rtt_us / n / 2 : 0);
        ntp_time_record_offset(inst, sender_fresh ? NTP_TIME_PEER : NTP_TIME_SAVED, offset);
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP_node: received node=%s fresh=%d offset=%lldus active_priority=%d",
            sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}), sender_fresh, (long long)offset, inst->ntp.source);
    }

    queue_dgram_free(entry); queue_entry_free(entry);

    // Предлагаем поправку узлу без неё либо свежую — узлу только с кэшем. Равный уровень не вызывает эхо.
    if ((!sender_synced && ntp_time_is_synced(inst)) || (!sender_fresh && inst->ntp.source >= NTP_TIME_PEER)) {
        send_time_sync(inst, conn);
    }

    // Повышение качества распространяется дальше. Обновления равного уровня не создают циклическую рассылку.
    if (inst->ntp.source > previous) {
        ntp_node_sync_peers(inst);
    }
}

void ntp_node_sync_peers(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->ntp_node.active) return;

    int sent = 0;
    for (struct ll_entry* entry = inst->connections->head; entry; entry = entry->next) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        if (!ce->conn->initialized || !ce->conn->links_up) continue;
        if (send_time_sync(inst, ce->conn) == 0) sent++;
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP_node: %s TIME_SYNC to %d peers",
        ntp_time_is_synced(inst) ? "broadcast" : "request fallback", sent);
}

int ntp_node_time_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: init requires instance and connections queue");
        return -1;
    }
    struct NTP_NODE_TIME* np = &inst->ntp_node;
    if (np->active) return 0;
    np->peer_count = 0;

    if (inst->api_bindings.callbacks[ETCP_ID_NTP_TIME]) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: packet ID=0x%02X already bound", ETCP_ID_NTP_TIME);
        return -1;
    }
    if (etcp_bind(inst, ETCP_ID_NTP_TIME, ntp_node_recv_cb) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: etcp_bind failed ID=0x%02X", ETCP_ID_NTP_TIME);
        return -1;
    }
    struct etcp_inst_cbk_entry* prev = inst->new_conn_cbks;
    etcp_add_new_conn_cbk(inst, ntp_node_on_new_conn, inst);
    if (inst->new_conn_cbks == prev) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP_node: cannot subscribe new connections");
        etcp_unbind(inst, ETCP_ID_NTP_TIME);
        return -1;
    }
    np->active = 1;
    for (struct ll_entry* e = inst->connections->head; e; e = e->next) {
        struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
        if (ntp_node_subscribe(conn, inst) != 0) {
            ntp_node_time_destroy(inst);
            return -1;
        }
    }
    for (struct ll_entry* e = inst->connections->head; e; e = e->next) {
        struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
        if (conn->initialized) ntp_node_on_conn_init(conn, ETCP_CBK_EVENT_INIT, inst);
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP_node: initialized (raw ETCP, ID=0x%02X)", ETCP_ID_NTP_TIME);
    return 0;
}

void ntp_node_time_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    struct NTP_NODE_TIME* np = &inst->ntp_node;
    if (!np->active) return;
    np->active = 0;

    etcp_unbind(inst, ETCP_ID_NTP_TIME);
    etcp_remove_new_conn_cbk(inst, ntp_node_on_new_conn, inst);
    for (struct ll_entry* e = inst->connections->head; e; e = e->next) {
        struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
        etcp_conn_remove_cbk(conn, ntp_node_on_conn_init, inst);
    }

    np->peer_count = 0;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP_node: destroyed");
}
