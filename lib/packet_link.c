#include "packet_link_backend.h"
#include "u_async.h"
#include "ll_queue.h"
#include "mem.h"
#include "debug_config.h"
#include <stdlib.h>

struct packet_link {
    struct UASYNC* ua;
    struct packet_link_driver driver;
    char* name;
    struct packet_link_callbacks callbacks;
    struct packet_link_state state;
    void *arg, *notification;
    unsigned active;
    int closing, rx_enabled, sending;
};

static void destroy_deferred(void* arg) {
    struct packet_link* link = arg;
    link->driver.destroy(link->driver.ctx);
    u_free(link->name);
    u_free(link);
}

static void release(struct packet_link* link) {
    if (--link->active || !link->closing) return;
    /* Backend мог вызвать receive из своего I/O callback: его стек тоже должен завершиться. */
    if (!uasync_call_soon(link->ua, link, destroy_deferred)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link: cannot defer destruction"); abort();
    }
}

static void notify(void* arg) {
    struct packet_link* link = arg;
    link->notification = NULL;
    link->active++;
    if (link->callbacks.state_changed) link->callbacks.state_changed(link, link->arg);
    release(link);
}

static int schedule(struct packet_link* link) {
    if (link->notification || link->closing) return 0;
    link->notification = uasync_call_soon(link->ua, link, notify);
    if (link->notification) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link: cannot schedule state notification");
    return -1;
}

static int valid_state(const struct packet_link_state* s) {
    return s && s->phase >= PACKET_LINK_CONNECTING && s->phase <= PACKET_LINK_TERMINATED &&
        s->max_packet && s->max_packet <= UINT16_MAX && (!s->tx_ready || s->phase == PACKET_LINK_OPEN);
}

static void driver_changed(void* arg);
static void driver_received(void* arg, struct ll_entry* packet);

struct packet_link* packet_link_bind(const struct packet_link_driver* driver,
    const struct packet_link_callbacks* callbacks, void* arg) {
    if (!driver || !driver->ua || !driver->name || !driver->name[0] || !driver->bind || !driver->get_state ||
        !driver->send || !driver->stop || !driver->destroy || !driver->set_rx_enabled || !callbacks || !callbacks->received ||
        driver->direction < PACKET_LINK_DIRECTION_UNKNOWN || driver->direction > PACKET_LINK_INCOMING) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link: invalid driver API"); return NULL;
    }
    struct packet_link_state initial = driver->get_state(driver->ctx);
    if (!valid_state(&initial) || initial.phase == PACKET_LINK_TERMINATED) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link %s: invalid initial state", driver->name); return NULL;
    }
    struct packet_link* link = u_calloc(1, sizeof(*link));
    if (!link) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link: allocation failed"); return NULL; }
    link->ua = driver->ua; link->driver = *driver; link->state = initial; link->callbacks = *callbacks; link->arg = arg;
    link->name = u_strdup(driver->name);
    if (!link->name || schedule(link)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link: cannot prepare binding"); u_free(link->name); u_free(link); return NULL;
    }
    link->driver.name = link->name;
    const struct packet_link_events events = {driver_changed, driver_received};
    if (driver->bind(driver->ctx, &events, link)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link %s: driver binding rejected", link->name);
        uasync_call_soon_cancel(link->ua, link->notification); u_free(link->name); u_free(link); return NULL;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "packet link=%p driver=%s direction=%d phase=%d max=%zu ready=%d",
        (void*)link, link->name, link->driver.direction, initial.phase, initial.max_packet, initial.tx_ready);
    return link;
}

const char* packet_link_name(const struct packet_link* link) { return link ? link->name : "unbound"; }

enum packet_link_direction packet_link_get_direction(const struct packet_link* link) {
    return link ? link->driver.direction : PACKET_LINK_DIRECTION_UNKNOWN;
}

struct packet_link_state packet_link_get_state(const struct packet_link* link) {
    if (!link || link->closing) return (struct packet_link_state){PACKET_LINK_TERMINATED, 0, 0, PACKET_LINK_CLOSED};
    return link->state;
}

static void driver_changed(void* arg) {
    struct packet_link* link = arg;
    if (!link || link->closing) return;
    struct packet_link_state snapshot = link->driver.get_state(link->driver.ctx);
    const struct packet_link_state* state = &snapshot;
    if (!valid_state(state) || state->max_packet != link->state.max_packet || (link->state.phase == PACKET_LINK_TERMINATED && state->phase != PACKET_LINK_TERMINATED)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link: invalid state transition"); return;
    }
    struct packet_link_state old = link->state;
    if (old.phase == state->phase && old.max_packet == state->max_packet && old.tx_ready == state->tx_ready && old.error == state->error) return;
    link->state = *state;
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "packet link=%p phase=%d ready=%d max=%zu error=%d",
        (void*)link, state->phase, state->tx_ready, state->max_packet, state->error);
    /* Потерять обязательное событие нельзя; синхронный callback нарушил бы контракт send. */
    if (schedule(link)) abort();
}

int packet_link_send(struct packet_link* link, struct ll_entry* packet) {
    if (!link || link->closing || link->state.phase == PACKET_LINK_TERMINATED) return PACKET_LINK_CLOSED;
    if (!packet || packet->owner || packet->next || packet->prev || !packet->dgram || !packet->len || packet->len > packet->memlen) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link: invalid TX packet"); return PACKET_LINK_INVALID;
    }
    if (packet->len > link->state.max_packet) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link: oversized TX size=%u max=%zu", packet->len, link->state.max_packet);
        return PACKET_LINK_TOO_LARGE;
    }
    if (!link->state.tx_ready || link->sending) return PACKET_LINK_WOULD_BLOCK;
    link->active++; link->sending = 1;
    int result = link->driver.send(link->driver.ctx, packet);
    link->sending = 0;
    release(link);
    return result;
}

void packet_link_set_rx_enabled(struct packet_link* link, int enabled) {
    if (!link || link->closing || link->rx_enabled == !!enabled) return;
    link->active++; link->rx_enabled = !!enabled;
    link->driver.set_rx_enabled(link->driver.ctx, !!enabled);
    release(link);
}

static void driver_received(void* arg, struct ll_entry* packet) {
    struct packet_link* link = arg;
    if (!packet || packet->owner || packet->next || packet->prev || !packet->dgram || !packet->len ||
        packet->len > packet->memlen || packet->len > link->state.max_packet ||
        link->closing || !link->rx_enabled || link->sending || link->state.phase != PACKET_LINK_OPEN) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet link %s: invalid RX bytes=%u capacity=%u queued=%d phase=%d enabled=%d sending=%d closing=%d",
            link->name, packet ? packet->len : 0, packet ? packet->memlen : 0, packet && packet->owner != NULL,
            link->state.phase, link->rx_enabled, link->sending, link->closing);
        if (packet) { queue_dgram_free(packet); queue_entry_free(packet); }
        return;
    }
    link->active++;
    link->callbacks.received(link, packet, link->arg);
    release(link);
}

void packet_link_close(struct packet_link* link) {
    if (!link || link->closing) return;
    link->active++; link->closing = 1;
    if (link->notification) { uasync_call_soon_cancel(link->ua, link->notification); link->notification = NULL; }
    link->driver.stop(link->driver.ctx);
    release(link);
}
