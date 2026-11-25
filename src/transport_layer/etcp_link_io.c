#include "etcp_link_io.h"
#include "etcp_connections.h"
#include "etcp.h"
#include "stcp_link.h"
#include "socks_client.h"
#include "packet_transport.h"
#include "../../lib/packet_link_backend.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if ETCP_LINK_SOCKET_NAME_SIZE != MAX_CONN_NAME_LEN
#error "link snapshot socket name must fit the complete socket name"
#endif

struct native_adapter {
    struct packet_link_events events;
    void* event_arg;
    struct packet_link_state state;
    struct ETCP_LINK* owner;
    struct ll_queue *tx, *rx;
    int rx_enabled;
};

struct etcp_link_io {
    struct packet_link* channel;
    struct native_adapter* native; /* Только платформенная привязка UDP/STCP. */
    struct ETCP_LINK* owner;
    int announced;
};

static int bind_native(void* arg, const struct packet_link_events* events, void* event_arg) {
    struct native_adapter* io = arg;
    if (io->event_arg) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet adapter already bound"); return -1; }
    io->events = *events; io->event_arg = event_arg;
    return 0;
}
static struct packet_link_state native_state(void* arg) {
    struct native_adapter *io = arg;
    struct packet_link_state state = io->state;
    if (io->owner->is_tcp) state.tx_ready = stcp_link_can_send(io->owner->tcp_link);
    return state;
}
static void publish(struct native_adapter* io, struct packet_link_state state) {
    io->state = state;
    if (io->events.state_changed) io->events.state_changed(io->event_arg);
}

static void free_packet(struct ll_entry* p) { queue_dgram_free(p); queue_entry_free(p); }

static void rx_drain(struct ll_queue* q, void* arg) {
    struct native_adapter* io = arg;
    if (!io->rx_enabled) return;
    struct ll_entry* p = queue_data_get(q);
    if (!p) { queue_resume_callback(q); return; }
    /* Получатель может закрыть owner и адаптер: продолжение планируем до callback. */
    queue_resume_callback(q);
    io->events.received(io->event_arg, p);
}

static int udp_send(struct native_adapter* io, struct ll_entry* p) {
    struct ETCP_LINK* link = io->owner;
    if (link->conn->socks_udp && !socks_udp_is_ready(link->conn->socks_udp)) return PACKET_LINK_WOULD_BLOCK;
    struct sockaddr_storage* addr = &link->remote_addr;
    socklen_t size = addr->ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    ssize_t n = etcp_udp_send(link, link->conn->fd, p->dgram, p->len, (struct sockaddr*)addr, size);
    if (n == p->len) { free_packet(p); return PACKET_LINK_OK; }
    int err = socket_get_error();
    if (n < 0 && (err == ERR_AGAIN || err == ERR_WOULDBLOCK)) return PACKET_LINK_WOULD_BLOCK;
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] UDP packet send failed path=%u size=%u result=%zd error=%d",
        link->etcp->log_name, link->local_link_id, p->len, n, err);
    return PACKET_LINK_ERROR;
}

void etcp_link_io_update(struct ETCP_LINK* link) {
    if (!link || !link->io || !link->io->native) return;
    struct native_adapter* io = link->io->native;
    struct packet_link_state state = {PACKET_LINK_OPEN, PACKET_DATA_SIZE, io->tx && !io->tx->count, 0};
    if (link->is_tcp) {
        state.phase = stcp_link_is_ready(link->tcp_link) ? PACKET_LINK_OPEN : PACKET_LINK_CONNECTING;
        state.tx_ready = state.phase == PACKET_LINK_OPEN && stcp_link_can_send(link->tcp_link);
    }
    publish(io, state);
}

static int adapter_send(void* arg, struct ll_entry* p) {
    struct native_adapter* io = arg;
    struct ETCP_LINK* link = io->owner;
    int result;
    if (link->is_tcp) {
        if (link->send_hook) {
            ssize_t n = link->send_hook(0, p->dgram, p->len, NULL, 0, link, link->send_hook_ctx);
            result = n >= 0 ? PACKET_LINK_OK : PACKET_LINK_ERROR;
            if (result == PACKET_LINK_OK) free_packet(p);
        } else result = stcp_link_send_entry(link->tcp_link, p);
    } else {
        if (io->tx->count) return PACKET_LINK_WOULD_BLOCK;
        result = udp_send(io, p);
        if (result == PACKET_LINK_WOULD_BLOCK) {
            /* Сохраняем целую датаграмму, а не считаем EAGAIN успешной передачей в сокет. */
            if (queue_data_put(io->tx, p)) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "UDP TX queue invariant failed"); abort(); }
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] UDP TX waits path=%u bytes=%u", link->etcp->log_name, link->local_link_id, p->len);
            if (!link->conn->socks_udp || socks_udp_is_ready(link->conn->socks_udp))
                uasync_set_socket_write(link->etcp->instance->ua, link->conn->socket_id, 1);
            result = PACKET_LINK_OK;
        }
    }
    etcp_link_io_update(link);
    return result;
}

static void rx_enabled(void* arg, int enabled) {
    struct native_adapter* io = arg;
    io->rx_enabled = enabled;
    if (io->owner->tcp_link) stcp_link_set_rx_enabled(io->owner->tcp_link, enabled);
    if (enabled) queue_resume_callback(io->rx);
}

static void stop(void* arg) {
    struct native_adapter* io = arg;
    memset(&io->events, 0, sizeof(io->events)); io->event_arg = NULL;
    queue_set_callback(io->rx, NULL, NULL);
    io->rx_enabled = 0;
    if (io->owner->tcp_link) stcp_link_set_rx_enabled(io->owner->tcp_link, 0);
}

static void destroy(void* arg) {
    struct native_adapter* io = arg;
    queue_clear(io->tx); queue_free(io->tx);
    queue_clear(io->rx); queue_free(io->rx);
    u_free(io);
}

static void changed(struct packet_link* channel, void* arg) {
    struct etcp_link_io* io = arg;
    struct packet_link_state state = packet_link_get_state(channel);
    if (state.phase == PACKET_LINK_TERMINATED) { etcp_link_close(io->owner); return; }
    struct ETCP_LINK* owner = io->owner;
    struct ETCP_CONN* conn = owner->etcp;
    conn->callbacks_running++; owner->active_users++;
    if (!io->native) {
        if (!io->announced) {
            io->announced = 1; etcp_fire_link_status_cbk(owner, -1, 0);
            if (!etcp_link_is_alive(conn, owner)) goto done;
        }
        if (state.phase == PACKET_LINK_CONNECTING && owner->link_state) {
            int old_state = owner->link_state, old_status = owner->link_status;
            void** timers[] = {&owner->init_timer, &owner->keepalive_timer, &owner->rx_timeout_timer};
            for (unsigned i = 0; i < sizeof(timers)/sizeof(timers[0]); i++) {
                if (*timers[i]) uasync_cancel_timeout(conn->instance->ua, *timers[i]);
                *timers[i] = NULL;
            }
            owner->initialized = owner->link_status = owner->recv_keepalive = owner->remote_keepalive = 0;
            owner->link_state = 0;
            packet_link_set_rx_enabled(channel, 0);
            etcp_fire_link_status_cbk(owner, old_state, old_status);
            if (etcp_link_is_alive(conn, owner)) etcp_on_link_down(conn, owner);
            goto done;
        }
        if (state.phase == PACKET_LINK_OPEN && !owner->link_state) {
            packet_link_set_rx_enabled(channel, 1);
            if (etcp_link_is_alive(conn, owner)) etcp_link_enter_init(owner);
            goto done;
        }
    }
    if (state.tx_ready) {
        if (!io->native) DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] carrier ready path=%u driver=%s input=%d send=%d token=%d",
            conn->log_name, owner->local_link_id, packet_link_name(channel), queue_entry_count(conn->input_queue),
            queue_entry_count(conn->input_send_q), conn->tx_token != NULL);
        etcp_conn_wake_sender(conn);
    }
done:
    etcp_link_release(owner); conn->callbacks_running--;
}

static void received(struct packet_link* channel, struct ll_entry* p, void* arg) {
    (void)channel;
    struct etcp_link_io* io = arg;
    io->owner->diagnostic_wire_rx_bytes += p->len;
    io->owner->etcp->diagnostic_wire_rx_bytes += p->len;
    io->owner->codec->receive(io->owner, p);
}

int etcp_link_io_create(struct ETCP_LINK* link) {
    struct native_adapter* io = u_calloc(1, sizeof(*io));
    if (!io) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet adapter allocation failed"); return -1; }
    io->owner = link;
    if (!link->is_tcp) io->tx = queue_new(link->etcp->instance->ua, 0, 0, 0, "link_tx");
    io->rx = queue_new(link->etcp->instance->ua, 0, 0, 0, "link_rx");
    if ((!link->is_tcp && !io->tx) || !io->rx) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet adapter queue allocation failed"); destroy(io); return -1;
    }
    io->state = (struct packet_link_state){link->is_tcp ? PACKET_LINK_CONNECTING : PACKET_LINK_OPEN,
        PACKET_DATA_SIZE, !link->is_tcp, 0};
    struct packet_link_driver driver = {io, link->etcp->instance->ua, link->is_tcp ? "STCP" : "UDP",
        bind_native, native_state, adapter_send, rx_enabled, stop, destroy,
        link->is_server ? PACKET_LINK_INCOMING : PACKET_LINK_OUTGOING};
    if (etcp_link_io_bind(link, &driver)) { destroy(io); return -1; }
    link->io->native = io;
    queue_set_callback(io->rx, rx_drain, io);
    packet_link_set_rx_enabled(link->io->channel, 1);
    return 0;
}

int etcp_link_io_bind(struct ETCP_LINK* link, const struct packet_link_driver* driver) {
    if (link->io || !driver || driver->ua != link->etcp->instance->ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet link binding: occupied link or different event loop"); return -1;
    }
    struct etcp_link_io* io = u_calloc(1, sizeof(*io));
    if (!io) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet binding allocation failed"); return -1; }
    io->owner = link;
    const struct packet_link_callbacks callbacks = {changed, received};
    io->channel = packet_link_bind(driver, &callbacks, io);
    if (!io->channel) { u_free(io); return -1; }
    link->io = io;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] packet path=%u driver=%s direction=%d", link->etcp->log_name,
        link->local_link_id, packet_link_name(io->channel), packet_link_get_direction(io->channel));
    return 0;
}

void etcp_link_io_destroy(struct ETCP_LINK* link) {
    if (!link || !link->io) return;
    struct etcp_link_io* io = link->io; link->io = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] packet path closed=%u driver=%s direction=%d", link->etcp->log_name,
        link->local_link_id, packet_link_name(io->channel), packet_link_get_direction(io->channel));
    packet_link_close(io->channel); u_free(io);
}

struct packet_link* etcp_link_io_get(struct ETCP_LINK* link) { return link && link->io ? link->io->channel : NULL; }
int etcp_link_io_ready(struct ETCP_LINK* link) {
    if (link && !link->is_tcp && link->conn && link->conn->transport && !link->send_hook) {
        if (link->conn->socks_udp && !socks_udp_is_ready(link->conn->socks_udp)) return 0;
        return packet_transport_ready(link->conn->transport, &link->remote_addr);
    }
    return link && (!link->io || packet_link_get_state(link->io->channel).tx_ready);
}

static int snapshot_address(const struct sockaddr_storage* source, struct etcp_link_address* out) {
    if (!source || !source->ss_family) return 0;
    if (source->ss_family == AF_INET) {
        const struct sockaddr_in* value = (const struct sockaddr_in*)source;
        out->family = 4; memcpy(out->bytes, &value->sin_addr, 4); out->port = ntohs(value->sin_port);
    } else if (source->ss_family == AF_INET6) {
        const struct sockaddr_in6* value = (const struct sockaddr_in6*)source;
        out->family = 6; memcpy(out->bytes, &value->sin6_addr, 16); out->port = ntohs(value->sin6_port);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "link snapshot: unsupported address family=%d", source->ss_family); return -1;
    }
    return 0;
}

int etcp_link_get_snapshot(const struct ETCP_LINK* link, struct etcp_link_snapshot* out) {
    if (!link || link->closing || !out) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "link snapshot: missing/closing link or output"); return -1; }
    struct etcp_link_snapshot s = {0};
    s.id = link->local_link_id; s.state = link->link_state; s.status = link->link_status;
    int external = link->io && !link->io->native;
    const char* protocol;
    if (external) {
        protocol = packet_link_name(link->io->channel);
        s.direction = packet_link_get_direction(link->io->channel);
    } else {
        s.has_ip = 1;
        s.direction = link->is_server ? PACKET_LINK_INCOMING : PACKET_LINK_OUTGOING;
        protocol = link->reality_set ? "TCP+REALITY" : link->is_tcp ? "TCP" : "UDP";
        const struct sockaddr_storage* local = NULL;
        if (link->conn) local = link->conn->interface_addr.ss_family ? &link->conn->interface_addr : &link->conn->local_addr;
        else if (link->is_tcp && link->tcp_link) local = stcp_link_get_local_addr(link->tcp_link);
        if (snapshot_address(local, &s.local) || snapshot_address(&link->remote_addr, &s.remote)) return -1;
        if (link->conn) snprintf(s.socket_name, sizeof(s.socket_name), "%s", link->conn->name);
    }
    if (snprintf(s.protocol, sizeof(s.protocol), "%s", protocol) >= (int)sizeof(s.protocol))
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "link snapshot: truncated driver name path=%u name=%s", s.id, protocol);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "link snapshot path=%u protocol=%s direction=%d ip=%u state=%u status=%u",
        s.id, s.protocol, s.direction, s.has_ip, s.state, s.status);
    *out = s;
    return 0;
}

int etcp_link_io_send(struct ETCP_LINK* link, const void* bytes, size_t size) {
    if (!link || !bytes || !size || size > PACKET_DATA_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet adapter invalid TX size=%zu", size); return -1;
    }
    if (!link->io && etcp_link_io_create(link)) return -1;
    if (!etcp_link_io_ready(link)) return -2;
    struct ll_entry* p = ll_alloc_lldgram(size);
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet adapter TX allocation failed size=%zu", size); return -1; }
    memcpy(p->dgram, bytes, size); p->len = size;
    int result = packet_link_send(link->io->channel, p);
    if (result == PACKET_LINK_OK) {
        link->diagnostic_wire_tx_bytes += size;
        link->etcp->diagnostic_wire_tx_bytes += size;
        return (int)size;
    }
    free_packet(p);
    return result == PACKET_LINK_WOULD_BLOCK ? -2 : -1;
}

void etcp_link_io_receive(struct ETCP_LINK* link, struct ll_entry* p) {
    struct native_adapter* io = link->io ? link->io->native : NULL;
    if (io && io->rx_enabled) { io->events.received(io->event_arg, p); return; }
    if (!io || io->rx->count >= 64) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "packet adapter RX full/unavailable path=%u", link->local_link_id); free_packet(p); return;
    }
    if (queue_data_put(io->rx, p)) DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet adapter RX insertion failed");
}

void etcp_link_io_socket_writable(socket_t socket, void* arg) {
    (void)socket;
    struct ETCP_SOCKET* sock = arg;
    uasync_set_socket_write(sock->instance->ua, sock->socket_id, 0);
    for (struct ll_entry* e = sock->links_queue->head; e; e = e->next) {
        struct ETCP_LINK* link = ((struct link_queue_entry*)e->data)->link;
        if (link->conn != sock || !link->io || !link->io->native || !link->io->native->tx || !link->io->native->tx->head) continue;
        struct native_adapter* io = link->io->native;
        struct ll_entry* p = queue_data_get(io->tx);
        int result = udp_send(io, p);
        if (result == PACKET_LINK_WOULD_BLOCK) {
            queue_data_put(io->tx, p);
            if (!sock->socks_udp || socks_udp_is_ready(sock->socks_udp))
                uasync_set_socket_write(sock->instance->ua, sock->socket_id, 1);
            return;
        }
        if (result != PACKET_LINK_OK) {
            free_packet(p);
            struct packet_link_state state = {PACKET_LINK_TERMINATED, PACKET_DATA_SIZE, 0, result};
            publish(io, state);
        } else etcp_link_io_update(link);
    }
}
