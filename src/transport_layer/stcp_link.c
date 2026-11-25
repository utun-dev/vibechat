// stcp_link.c — STCP link management implementation
#include "stcp_link.h"
#include "etcp_link_io.h"
#include "../../lib/packet_link.h"
#include "call_ring.h"
#include "stcp.h"
#include "stream_transport.h"
#include "stcp_server.h"
#include "stcp_client.h"
#include "secure_channel.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_router.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"
#include <stdlib.h>
#include <string.h>

struct stcp_server {
    struct stcp_server *next;     // linked list in UTUN_INSTANCE
    struct stcp_server *srv;      // stcp_server from stcp_server.h
    struct UTUN_INSTANCE *inst;
    stcp_server_on_link_cb on_link;
    struct ETCP_SOCKET *tcp_sock;
};

struct stcp_link {
    struct stcp_client *cli;
    struct stcp_conn *conn;

    struct UTUN_INSTANCE *inst;   // для rx-диспатча (из etcp_conn->instance)
    struct UASYNC *ua;            // event loop (из inst->ua)

    struct ETCP_CONN *etcp_conn;   // parent ETCP_CONN (for rx dispatch / etcp_send compat)
    struct ETCP_LINK *etcp_link;   // owning ETCP_LINK (for etcp_conn_input pkt->link)

    uint8_t ready;
    uint8_t closing;
    uint8_t rx_enabled;

    stcp_link_cb on_ready_cb;
    void *ready_arg;
    void (*on_close_cb)(struct stcp_link *link, int err, void *arg);
    void *close_arg;

    struct ll_queue *rx_queue;     // Обе очереди принадлежат адаптеру, conn только заимствует их.
    struct ll_queue *tx_queue;

    uint64_t peer_reset_id;            // received from peer during handshake (reset epoch)
    uint8_t peer_device_type;          // CLIENT_TYPE_* from peer handshake
    uint16_t peer_keepalive_interval;  // keepalive interval from peer handshake
};

// ====== rx dispatch ======

static void link_rx_cb(struct ll_queue *q, void *arg) {
    struct stcp_link *link = (struct stcp_link *)arg;
    if (link->closing || !link->rx_enabled) return;
    struct ll_entry *e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); return; }

    if (link->etcp_conn && link->etcp_link && link->inst) {
        if (e->len > PACKET_DATA_SIZE) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_link rx oversized pkt: %u > %u", e->len, (unsigned)PACKET_DATA_SIZE);
            queue_dgram_free(e); queue_entry_free(e); queue_resume_callback(q);
            if (link->conn) stcp_conn_do_close(link->conn, EMSGSIZE);
            return;
        }
        etcp_link_io_receive(link->etcp_link, e);
    } else {
        struct UTUN_INSTANCE *inst = link->inst;
        uint8_t id = (e->dgram && e->len > 0) ? e->dgram[0] : ETCP_ID_DATA;
        if (inst && inst->api_bindings.callbacks[id])
            inst->api_bindings.callbacks[id](link->etcp_conn ? link->etcp_conn : NULL, e);
        else if (inst && inst->api_bindings.callbacks[ETCP_ID_DATA])
            inst->api_bindings.callbacks[ETCP_ID_DATA](link->etcp_conn ? link->etcp_conn : NULL, e);
        else { queue_dgram_free(e); queue_entry_free(e); }
    }
    if (!link->closing) {
        queue_resume_callback(q);
        if (link->conn) stcp_rx_resume_if_needed(link->conn);
    }
}

// ====== STCP close → link down ======

static void stcp_link_on_stcp_close(struct stcp_conn *conn, int err, void *arg) {
    struct stcp_link *link = (struct stcp_link *)arg;
    callring_put(CR_SLINK_ONSTCP, (uintptr_t)conn, (uintptr_t)arg);
    link->ready = 0;
    link->conn = NULL; /* Серверный conn освобождается после возврата из этого callback. */
    conn->tx_ready = NULL; conn->tx_ready_arg = NULL;
    conn->rx_queue = conn->tx_queue = NULL;
    queue_set_callback(link->rx_queue, NULL, NULL);
    queue_set_callback(link->tx_queue, NULL, NULL);
    queue_set_on_get(link->tx_queue, NULL, NULL);
    void (*callback)(struct stcp_link*, int, void*) = link->on_close_cb;
    void *callback_arg = link->close_arg;
    link->on_close_cb = NULL; link->close_arg = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp adapter closed: link=%p error=%d cancelling=%u", (void*)link, err, link->closing);
    if (!link->closing && callback) callback(link, err, callback_arg);
    /* Callback мог вызвать close; после уведомления адаптер больше не используем. */
}

static void tx_dequeued(struct ll_queue *q, void *arg) {
    (void)q;
    struct stcp_link *link = arg;
    if (!link->closing && link->etcp_link) etcp_link_io_update(link->etcp_link);
}

static void tx_ready(void *arg) {
    struct stcp_link *link = arg;
    if (!link->closing && link->etcp_link) etcp_link_io_update(link->etcp_link);
}

static int stcp_link_init_queues(struct stcp_link *link) {
    link->rx_enabled = 1;
    link->rx_queue = queue_new(link->ua, 0, 0, 0, "stcp_rx");
    link->tx_queue = queue_new(link->ua, 0, 0, 0, "stcp_tx");
    if (!link->rx_queue || !link->tx_queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP adapter queue allocation failed link=%p RX=%p TX=%p",
            (void*)link, (void*)link->rx_queue, (void*)link->tx_queue);
        return -1;
    }
    queue_set_callback(link->rx_queue, link_rx_cb, link);
    queue_set_threshold(link->rx_queue, STCP_RX_QUEUE_MAX_PACKETS, STCP_RX_QUEUE_MAX_BYTES);
    queue_set_threshold(link->tx_queue, 1, 0);
    queue_set_on_get(link->tx_queue, tx_dequeued, link);
    queue_set_waiter_defer(link->tx_queue, 1);
    stcp_conn_set_rx_queue(link->conn, link->rx_queue);
    stcp_conn_set_tx_queue(link->conn, link->tx_queue);
    link->conn->tx_ready = tx_ready; link->conn->tx_ready_arg = link;
    return 0;
}

// ====== server accept → link ======

static void server_accept_cb(struct stcp_conn *conn, void *arg) {
    struct stcp_server *ss = (struct stcp_server *)arg;
    struct stcp_link *link = u_calloc(1, sizeof(struct stcp_link));
    if (!link) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP server adapter allocation failed"); stcp_conn_do_close(conn, ENOMEM); return; }
    link->inst = ss->inst;
    link->ua = ss->inst->ua;
    link->ready = 1;
    link->conn = conn;
    link->peer_reset_id = conn->peer_reset_id;
    link->peer_device_type = conn->peer_device_type;
    link->peer_keepalive_interval = conn->peer_keepalive_interval;

    stcp_conn_set_on_close(conn, stcp_link_on_stcp_close, link);

    if (stcp_link_init_queues(link) < 0) { stcp_link_close(link); return; }

    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp_link: server accepted connection");
    if (ss->on_link) ss->on_link(link, ss->tcp_sock);
}

// ====== client connect → link ======

static void client_ready_cb(struct stcp_conn *conn, void *arg) {
    struct stcp_link *link = (struct stcp_link *)arg;
    if (!conn || link->closing) return;
    link->conn = conn;
    link->peer_reset_id = conn->peer_reset_id;
    link->peer_device_type = conn->peer_device_type;
    link->peer_keepalive_interval = conn->peer_keepalive_interval;

    if (stcp_link_init_queues(link) < 0) { stcp_conn_do_close(conn, ENOMEM); return; }
    link->ready = 1;

    {
        const struct sockaddr_storage *ra = link->etcp_link ? &link->etcp_link->remote_addr : NULL;
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp_link: client handshake OK [%s] link=%d addr=%s rid=%016llx",
                   link->etcp_conn ? link->etcp_conn->log_name : "?",
                   link->etcp_link ? link->etcp_link->local_link_id : -1,
                   ra ? sockaddr_storage_to_str(ra).str : "?",
                   (unsigned long long)link->peer_reset_id);
    }
    if (link->etcp_link) etcp_link_enter_ready_tcp(link->etcp_link);
    if (link->closing || !link->etcp_conn || link->etcp_conn->close_requested) return;
    if (link->on_ready_cb) link->on_ready_cb(link, link->ready_arg);
}

// ====== API ======

struct stcp_server *stcp_server_listen(struct stcp_link_config *cfg, uint16_t port,
                                        stcp_server_on_link_cb on_link, struct ETCP_SOCKET *tcp_sock) {
    if (!cfg || !cfg->inst) return NULL;
    struct stcp_server *ss = u_calloc(1, sizeof(struct stcp_server));
    if (!ss) return NULL;
    ss->inst = cfg->inst;
    ss->on_link = on_link;
    ss->tcp_sock = tcp_sock;
    ss->srv = stcp_server_create(cfg->inst->ua, port, &cfg->inst->my_keys, cfg->inst->my_ed25519_pubkey,
                                 cfg->inst, server_accept_cb, ss, NULL, NULL, cfg->listen_family);
    if (!ss->srv) { int error = socket_get_error(); u_free(ss); socket_set_error(error); return NULL; }
    uint16_t bound_port = stcp_server_get_port(ss->srv);
    if (!bound_port) { stcp_server_destroy(ss->srv); u_free(ss); return NULL; }
    if (tcp_sock) {
        if (tcp_sock->local_addr.ss_family == AF_INET) {
            ((struct sockaddr_in*)&tcp_sock->local_addr)->sin_port = htons(bound_port);
            ((struct sockaddr_in*)&tcp_sock->interface_addr)->sin_port = htons(bound_port);
        } else if (tcp_sock->local_addr.ss_family == AF_INET6) {
            ((struct sockaddr_in6*)&tcp_sock->local_addr)->sin6_port = htons(bound_port);
            ((struct sockaddr_in6*)&tcp_sock->interface_addr)->sin6_port = htons(bound_port);
        }
    }
    if (cfg->reality_enabled && cfg->inst && cfg->inst->config)
        stcp_server_set_reality(ss->srv, &cfg->inst->config->global.reality);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "listen requested_port=%u bound_port=%u", port, bound_port);
    return ss;
}

uint16_t stcp_link_server_get_port(const struct stcp_server *ss) {
    if (!ss || !ss->srv) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp link server: missing listener"); return 0; }
    return stcp_server_get_port(ss->srv);
}

void stcp_link_server_destroy(struct stcp_server *ss) {
    if (!ss) return;
    if (ss->srv) stcp_server_destroy(ss->srv);
    u_free(ss);
}

struct stcp_link *stcp_link_connect(struct ETCP_LINK *etcp_link,
                                    struct sockaddr_storage *remote_addr, uint16_t port) {
    if (!etcp_link || !etcp_link->etcp || !remote_addr)
        return NULL;
    struct ETCP_CONN *etcp = etcp_link->etcp;
    struct UTUN_INSTANCE *inst = etcp->instance;

    int family = remote_addr->ss_family;
    char addr_str[64];
    uint16_t rport = port;
    if (family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)remote_addr;
        inet_ntop(AF_INET, &sa->sin_addr, addr_str, sizeof(addr_str));
        if (!rport) rport = ntohs(sa->sin_port);
    } else if (family == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)remote_addr;
        getnameinfo((struct sockaddr*)sa6, sizeof(*sa6), addr_str, sizeof(addr_str), NULL, 0, NI_NUMERICHOST);
        if (!rport) rport = ntohs(sa6->sin6_port);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_link: unsupported address family %d", family);
        return NULL;
    }

    struct stcp_link *link = u_calloc(1, sizeof(struct stcp_link));
    if (!link) return NULL;
    link->inst = inst;
    link->ua = inst->ua;
    link->etcp_conn = etcp;
    link->etcp_link = etcp_link;

    struct sockaddr_storage bind_addr;
    const struct sockaddr_storage *local_addr = NULL;
    if (etcp_link->conn && etcp_link->conn->interface_addr.ss_family) {
        memcpy(&bind_addr, &etcp_link->conn->interface_addr, sizeof(bind_addr));
        if (bind_addr.ss_family == AF_INET6) ((struct sockaddr_in6*)&bind_addr)->sin6_port = 0;
        else ((struct sockaddr_in*)&bind_addr)->sin_port = 0;
        local_addr = &bind_addr;
    }

    const uint8_t *pubkey = etcp->crypto_ctx.peer_public_key;

    // reality-камуфляж на клиенте: только если у пира есть реклама reality-сокета в BGP
    // (etcp_link->reality_set). Глобальный [reality] для исходящих не используется.
    const struct reality_client_config *reality = etcp_link->reality_set ? &etcp_link->reality : NULL;

    // SOCKS5-прокси: глобальная секция [socks]. Если включена — исходящие TCP-линки идут через прокси.
    struct socks_cfg socks_buf;
    const struct socks_cfg *socks = NULL;
    if (inst->config && inst->config->global.socks_enabled &&
        inst->config->global.socks_host[0] && inst->config->global.socks_port) {
        memset(&socks_buf, 0, sizeof(socks_buf));
        strncpy(socks_buf.host, inst->config->global.socks_host, sizeof(socks_buf.host) - 1);
        socks_buf.port = inst->config->global.socks_port;
        strncpy(socks_buf.user, inst->config->global.socks_username, sizeof(socks_buf.user) - 1);
        strncpy(socks_buf.pass, inst->config->global.socks_password, sizeof(socks_buf.pass) - 1);
        socks = &socks_buf;
    }

    link->cli = stcp_client_connect(inst->ua, addr_str, rport, &inst->my_keys, pubkey,
                                     inst->my_ed25519_pubkey,
                                     etcp->got_initial_pkt, etcp,
                                     inst->client_type, inst->keepalive_interval,
                                     0,
                                     client_ready_cb, link, NULL, NULL, NULL, NULL, local_addr, 0, reality, socks, inst);
    if (!link->cli) { u_free(link); return NULL; }
    link->conn = stcp_client_get_conn(link->cli);
    if (!link->conn || link->conn->state == STCP_STATE_CLOSED || link->conn->state == STCP_STATE_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP adapter startup failed before return target=%s:%u", addr_str, rport);
        stcp_client_destroy(link->cli); u_free(link); return NULL;
    }
    /* Установить до первого poll: закрытие connect/handshake также должно достигать владельца. */
    stcp_conn_set_on_close(link->conn, stcp_link_on_stcp_close, link);

    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp_link: connecting to %s:%u pubkey=%016llx bind=%s",
               addr_str, rport, (unsigned long long)*(const uint64_t*)pubkey,
               local_addr ? sockaddr_storage_to_str(local_addr).str : "auto");
    return link;
}

/* Освобождение очереди с предварительным дренажем записей: queue_free не
 * освобождает элементы (они должны извлекаться отдельно), иначе записи и их
 * dgram-буферы утекают при обрыве с данными в полёте. */
static void stcp_drain_free_queue(struct ll_queue *q) {
    if (!q) return;
    struct ll_entry *e;
    while ((e = queue_data_get(q)) != NULL) {
        queue_dgram_free(e);
        queue_entry_free(e);
    }
    queue_free(q);
}

static void stcp_link_close_impl(void *arg) {
    struct stcp_link *link = (struct stcp_link *)arg;
    callring_put(CR_SLINK_IMPL, (uintptr_t)link, (uintptr_t)(link->conn));

    struct stcp_conn *conn = link->conn;
    if (conn) {
        conn->rx_queue = conn->tx_queue = NULL;
        conn->tx_ready = NULL; conn->tx_ready_arg = NULL;
        conn->on_close = NULL; conn->close_arg = NULL;
    }
    stcp_drain_free_queue(link->rx_queue); stcp_drain_free_queue(link->tx_queue);
    if (link->cli) stcp_client_destroy(link->cli);
    else if (conn) stcp_conn_do_close(conn, 0);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp adapter freed: link=%p client=%u", (void*)link, link->cli != NULL);
    u_free(link);
}

void stcp_link_close(struct stcp_link *link) {
    if (!link) return;
    if (link->closing) return;
    link->closing = 1;
    link->ready = 0;
    link->on_ready_cb = NULL; link->ready_arg = NULL;
    queue_set_callback(link->rx_queue, NULL, NULL);
    queue_set_callback(link->tx_queue, NULL, NULL);
    queue_set_on_get(link->tx_queue, NULL, NULL);
    callring_put(CR_SLINK_CLOSE, (uintptr_t)link, (uintptr_t)(link->etcp_link));
    /* Обрыв вперёд-ссылки родителя: после отложенного free (close_impl)
     * etcp_link->tcp_link не должен остаться висячим. */
    if (link->etcp_link && link->etcp_link->tcp_link == link) link->etcp_link->tcp_link = NULL;
    link->etcp_conn = NULL;
    link->etcp_link = NULL;
    /* Явный teardown гасит «внезапное» уведомление: отложенный stcp_link_close_impl
     * через stcp_conn_do_close → stcp_link_on_stcp_close не должен перевызвать
     * on_close_cb с уже освобождённым ETCP_LINK. */
    link->on_close_cb = NULL;
    link->close_arg = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp_link_close: link=%p closing=%d", (void*)link, link->closing);

    if (link->conn) {
        struct stcp_conn *c = link->conn;
        c->tx_ready = NULL; c->tx_ready_arg = NULL;
        if (c->transport) stream_transport_pause(c->transport, 1);
        if (c->hs_timer) { uasync_cancel_timeout(link->ua, c->hs_timer); c->hs_timer = NULL; }
        if (c->socket_id) {
            uasync_set_socket_read(link->ua, c->socket_id, 0);
            uasync_set_socket_write(link->ua, c->socket_id, 0);
        }
    }
    if (!uasync_call_soon(link->ua, link, stcp_link_close_impl)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP adapter mandatory teardown scheduling failed link=%p", (void*)link);
        abort();
    }
}

int stcp_link_can_send(struct stcp_link *link) {
    return link && !link->closing && link->ready && link->conn && link->tx_queue && !link->tx_queue->count &&
        (!link->conn->transport || (!link->conn->pending_head && stream_transport_ready(link->conn->transport)));
}

int stcp_link_send_entry(struct stcp_link *link, struct ll_entry *packet) {
    if (!link || link->closing || !link->ready || !link->conn) return PACKET_LINK_CLOSED;
    if (!packet || packet->owner || packet->next || packet->prev || !packet->dgram || !packet->len) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP invalid TX packet"); return PACKET_LINK_INVALID;
    }
    if (!stcp_link_can_send(link)) return PACKET_LINK_WOULD_BLOCK;
    if (link->conn->transport) {
        const struct ETCP_PADDING *profile = link->etcp_conn ? &link->etcp_conn->padding : &link->inst->padding;
        int padding = link->conn->reality_io ? etcp_padding_size(profile, ETCP_PADDING_MAX) : 0;
        int rc = padding < 0 ? -1 : stream_transport_send(link->conn->transport, packet->dgram, packet->len, padding);
        if (rc) return rc == -2 ? PACKET_LINK_WOULD_BLOCK : PACKET_LINK_ERROR;
        queue_dgram_free(packet); queue_entry_free(packet); return PACKET_LINK_OK;
    }
    /* Очередь без hash/size_limit: после проверок отказ put означает нарушение инварианта,
     * не recoverable отказ send (ll_queue при таком отказе уже освобождает entry). */
    if (queue_data_put(link->tx_queue, packet)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP packet TX queue invariant failed"); abort();
    }
    return PACKET_LINK_OK;
}

void stcp_link_set_rx_enabled(struct stcp_link *link, int enabled) {
    if (!link || link->closing) return;
    link->rx_enabled = !!enabled;
    if (enabled) queue_resume_callback(link->rx_queue);
}

int stcp_link_send(struct stcp_link *link, const uint8_t *data, size_t len) {
    if (!stcp_link_can_send(link) || !data || !len || len > STCP_MAX_MSG_SIZE) return -1;
    struct ll_entry *e = ll_alloc_lldgram(len);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP TX allocation failed len=%zu", len); return -1; }
    memcpy(e->dgram, data, len); e->len = len;
    int result = stcp_link_send_entry(link, e);
    if (result == PACKET_LINK_OK) return 0;
    queue_dgram_free(e); queue_entry_free(e);
    return -1;
}

int stcp_link_is_ready(struct stcp_link *link) {
    return link && !link->closing ? link->ready : 0;
}

int stcp_link_uses_reality(struct stcp_link* link) {
    return link && !link->closing && link->conn && link->conn->reality_io;
}

struct ETCP_CONN *stcp_link_get_etcp_conn(struct stcp_link *link) {
    return link ? link->etcp_conn : NULL;
}

struct ETCP_LINK *stcp_link_get_etcp_link(struct stcp_link *link) {
    return link && !link->closing ? link->etcp_link : NULL;
}

void stcp_link_set_etcp_conn(struct stcp_link *link, struct ETCP_CONN *conn) {
    if (!link) return;
    link->etcp_conn = conn;
}

void stcp_link_set_etcp_link(struct stcp_link *link, struct ETCP_LINK *elink) {
    if (!link) return;
    link->etcp_link = elink;
}

const struct sockaddr_storage *stcp_link_get_remote_addr(struct stcp_link *link) {
    return link && link->etcp_link ? &link->etcp_link->remote_addr : NULL;
}

static struct sockaddr_storage g_peer_addr;
const struct sockaddr_storage *stcp_link_get_peer_addr(struct stcp_link *link) {
    if (!link || !link->conn || link->conn->sock == SOCKET_INVALID) return NULL;
    memset(&g_peer_addr, 0, sizeof(g_peer_addr));
    socklen_t slen = sizeof(g_peer_addr);
    if (getpeername(link->conn->sock, (struct sockaddr*)&g_peer_addr, &slen) == 0)
        return &g_peer_addr;
    return NULL;
}

static struct sockaddr_storage g_local_addr;
const struct sockaddr_storage *stcp_link_get_local_addr(struct stcp_link *link) {
    if (!link || !link->conn || link->conn->sock == SOCKET_INVALID) return NULL;
    memset(&g_local_addr, 0, sizeof(g_local_addr));
    socklen_t slen = sizeof(g_local_addr);
    if (getsockname(link->conn->sock, (struct sockaddr*)&g_local_addr, &slen) == 0)
        return &g_local_addr;
    return NULL;
}

void stcp_link_set_on_ready(struct stcp_link *link, stcp_link_cb cb, void *arg) {
    if (!link) return;
    link->on_ready_cb = cb;
    link->ready_arg = arg;
}

void stcp_link_set_on_close(struct stcp_link *link, void (*cb)(struct stcp_link *link, int err, void *arg), void *arg) {
    if (!link) return;
    link->on_close_cb = cb;
    link->close_arg = arg;
}

const uint8_t *stcp_link_get_peer_pubkey(struct stcp_link *link) {
    return link && link->conn ? link->conn->peer_pubkey : NULL;
}

const uint8_t *stcp_link_get_peer_ed25519_pubkey(struct stcp_link *link) {
    return link && link->conn && link->conn->peer_ed25519_set ? link->conn->peer_ed25519_pubkey : NULL;
}

uint64_t stcp_link_get_peer_reset_id(struct stcp_link *link) {
    return link ? link->peer_reset_id : 0;
}

uint8_t stcp_link_get_peer_device_type(struct stcp_link *link) {
    return link ? link->peer_device_type : 0;
}

uint16_t stcp_link_get_peer_keepalive_interval(struct stcp_link *link) {
    return link ? link->peer_keepalive_interval : 0;
}

void stcp_server_list_add(struct UTUN_INSTANCE *inst, struct stcp_server *srv) {
    if (!inst || !srv) return;
    srv->next = inst->stcp_servers;
    inst->stcp_servers = srv;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp_server_list_add: %p total=%d", (void*)srv, stcp_server_list_count(inst));
}

void stcp_server_list_remove(struct UTUN_INSTANCE *inst, struct stcp_server *srv) {
    if (!inst || !srv) return;
    struct stcp_server **pp = &inst->stcp_servers;
    while (*pp) {
        if (*pp == srv) { *pp = (*pp)->next; DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp_server_list_remove: %p total=%d", (void*)srv, stcp_server_list_count(inst)); return; }
        pp = &(*pp)->next;
    }
    DEBUG_WARN(DEBUG_CATEGORY_ETCP, "stcp_server_list_remove: not found %p", (void*)srv);
}

void stcp_server_list_destroy_all(struct UTUN_INSTANCE *inst) {
    if (!inst) return;
    int count = 0;
    while (inst->stcp_servers) {
        struct stcp_server *next = inst->stcp_servers->next;
        stcp_link_server_destroy(inst->stcp_servers);
        inst->stcp_servers = next;
        count++;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp_server_list_destroy_all: destroyed %d servers", count);
}

int stcp_server_list_count(struct UTUN_INSTANCE *inst) {
    if (!inst) return 0;
    int n = 0;
    for (struct stcp_server *s = inst->stcp_servers; s; s = s->next) n++;
    return n;
}

struct stcp_server *stcp_server_list_find_by_sock(struct UTUN_INSTANCE *inst, const struct ETCP_SOCKET *sock) {
    if (!inst || !sock) return NULL;
    for (struct stcp_server *s = inst->stcp_servers; s; s = s->next)
        if (s->tcp_sock == sock) return s;
    return NULL;
}
