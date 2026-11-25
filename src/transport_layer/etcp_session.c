#include "etcp_session.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include <string.h>

#define SESSION_RETRY_TB 5000
#define SESSION_TIMEOUT_TB 100000

static uint64_t read_id(const uint8_t* p) { uint64_t v; memcpy(&v,p,8); return be64toh(v); }
static void write_id(uint8_t* p, uint64_t v) { v = htobe64(v); memcpy(p,&v,8); }
static void session_schedule(struct ETCP_CONN* c, int initial);
void etcp_session_encode(uint8_t* data, uint8_t code, uint64_t sender, uint64_t target) {
    data[0] = code; write_id(data+1,sender); write_id(data+9,target);
}

static void session_send(struct ETCP_CONN* c, uint8_t code, uint64_t peer, uint64_t cookie) {
    for (struct ETCP_LINK* link = c->links; link; link = link->next) {
        if (!link->initialized) continue;
        struct ETCP_DGRAM* pkt = memory_pool_alloc(c->instance->pkt_pool);
        if (!pkt) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP,"[%s] session: packet allocation failed",c->log_name); return; }
        memset(pkt, 0, sizeof(*pkt));
        pkt->link = link; pkt->data_len = ETCP_SESSION_CONTROL_SIZE; pkt->noencrypt_len = 0;
        etcp_session_encode(pkt->data,code,c->reset_id,peer); write_id(pkt->data+17,cookie);
        if (etcp_encrypt_send(pkt) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_ETCP,"[%s] session: send failed code=%u link=%u",c->log_name,code,link->local_link_id);
        memory_pool_free(c->instance->pkt_pool,pkt);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP,"[%s] session TX code=%u local=%016llx peer=%016llx cookie=%016llx",
        c->log_name,code,(unsigned long long)c->reset_id,(unsigned long long)peer,(unsigned long long)cookie);
}

void etcp_session_cancel(struct ETCP_CONN* c) {
    if (c->session_timer) { uasync_cancel_timeout(c->instance->ua,c->session_timer); c->session_timer = NULL; }
    c->session_candidate = c->session_challenge = 0;
}

static void session_tick(void* arg) {
    struct ETCP_CONN* c = arg;
    c->session_timer = NULL;
    if (c->close_requested) return;
    if (get_time_tb() - c->session_started >= SESSION_TIMEOUT_TB) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP,"[%s] session confirmation timeout required=%u candidate=%016llx",c->log_name,
            c->session_required,(unsigned long long)c->session_candidate);
        etcp_session_cancel(c);
        if (c->session_required) etcp_connection_close(c);
        return;
    }
    if (c->session_required) session_send(c,ETCP_SESSION_HELLO,c->peer_reset_id,0);
    if (c->session_candidate) session_send(c,ETCP_SESSION_CHALLENGE,c->session_candidate,c->session_challenge);
    session_schedule(c, 0);
}

static void session_schedule(struct ETCP_CONN* c, int initial) {
    if (c->session_timer || c->close_requested) return;
    if (initial) c->session_started = get_time_tb();
    int delay_tb = initial ? 1 : SESSION_RETRY_TB;
    c->session_timer = uasync_set_timeout(c->instance->ua, delay_tb, c, session_tick, "etcp_session");
    if (!c->session_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] session timer OOM delay_tb=%d required=%u; closing",
            c->log_name, delay_tb, c->session_required);
        c->callbacks_running++; etcp_connection_close(c); c->callbacks_running--;
    }
}

void etcp_session_start(struct ETCP_CONN* c) {
    etcp_session_cancel(c); c->session_required = 1;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP,"[%s] session negotiation started local=%016llx peer=%016llx",c->log_name,
        (unsigned long long)c->reset_id,(unsigned long long)c->peer_reset_id);
    session_schedule(c, 1);
}

void etcp_session_observe(struct ETCP_CONN* c, uint64_t peer_epoch) {
    if (!peer_epoch || c->close_requested) return;
    if (c->session_candidate != peer_epoch) {
        c->session_candidate = peer_epoch;
        do {
            if (random_bytes((uint8_t*)&c->session_challenge,8) != 0) {
                c->session_candidate = 0;
                DEBUG_ERROR(DEBUG_CATEGORY_ETCP,"[%s] session challenge RNG failed",c->log_name); return;
            }
        } while (!c->session_challenge);
    }
    session_schedule(c, 1);
}

int etcp_session_receive(struct ETCP_CONN* c, const uint8_t* data, size_t len) {
    if (!len || data[0] < ETCP_SESSION_HELLO || data[0] > ETCP_SESSION_CONFIRM) return 0;
    if (len != ETCP_SESSION_CONTROL_SIZE || c->close_requested) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP,"[%s] invalid session control size=%zu",c->log_name,len); return 1;
    }
    uint64_t peer = read_id(data+1), target = read_id(data+9), cookie = read_id(data+17);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP,"[%s] session RX code=%u peer=%016llx target=%016llx cookie=%016llx",
        c->log_name,data[0],(unsigned long long)peer,(unsigned long long)target,(unsigned long long)cookie);
    if (!peer) { DEBUG_WARN(DEBUG_CATEGORY_ETCP,"[%s] session: zero peer epoch",c->log_name); return 1; }
    if (data[0] == ETCP_SESSION_HELLO) {
        etcp_session_observe(c,peer);
        if (c->session_challenge) session_send(c,ETCP_SESSION_CHALLENGE,peer,c->session_challenge);
    } else if (target != c->reset_id || !cookie) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP,"[%s] session: stale target or empty cookie",c->log_name);
    } else if (data[0] == ETCP_SESSION_CHALLENGE) {
        session_send(c,ETCP_SESSION_CONFIRM,peer,cookie);
        // Встречный challenge отправляет таймер: немедленный ответ зацикливает обмен при потере CONFIRM.
        if (c->session_required || peer != c->peer_reset_id) etcp_session_observe(c,peer);
    } else if (peer == c->session_candidate && cookie == c->session_challenge) {
        etcp_session_cancel(c);
        if (peer != c->peer_reset_id) etcp_conn_reinit_id(c,"confirmed peer epoch",c->reset_id);
        c->peer_reset_id = peer; c->session_required = 0;
        DEBUG_INFO(DEBUG_CATEGORY_ETCP,"[%s] session confirmed local=%016llx peer=%016llx",c->log_name,
            (unsigned long long)c->reset_id,(unsigned long long)peer);
        etcp_conn_ready(c);
    } else {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP,"[%s] session: stale confirmation",c->log_name);
    }
    return 1;
}

int etcp_session_accept_data(struct ETCP_CONN* c, const uint8_t* data, size_t len) {
    if (len <= ETCP_SESSION_HEADER_SIZE || data[0] != ETCP_SESSION_DATA || data[ETCP_SESSION_HEADER_SIZE] > 1) {
        c->session_data_errors++;
        if (c->session_data_errors <= 3 || c->session_data_errors % 1024 == 0) {
            int code = len ? data[0] : -1;
            int section = len > ETCP_SESSION_HEADER_SIZE ? data[ETCP_SESSION_HEADER_SIZE] : -1;
            const char* reason = len <= ETCP_SESSION_HEADER_SIZE ? "short header" :
                (code != ETCP_SESSION_DATA ? "missing session envelope" : "invalid stream section");
            DEBUG_WARN(DEBUG_CATEGORY_ETCP,
                "[%s] invalid session data: %s size=%zu code=%d expected=%u section=%d count=%llu",
                c->log_name, reason, len, code, ETCP_SESSION_DATA, section, (unsigned long long)c->session_data_errors);
        }
        return 0;
    }
    uint64_t peer = read_id(data+1), target = read_id(data+9);
    if (peer != c->peer_reset_id || target != c->reset_id || c->reinit_pending || !c->initialized) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP,"[%s] discard stale data peer=%016llx target=%016llx pending=%u",c->log_name,
            (unsigned long long)peer,(unsigned long long)target,c->reinit_pending);
        return 0;
    }
    return 1;
}
