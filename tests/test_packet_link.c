/* Контракт packet_link: владение, занятость, объединение событий, RX pause и teardown внутри callbacks. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../lib/packet_link_backend.h"
#include "../lib/ll_queue.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    struct packet_link* link;
    struct packet_link_events events;
    void* event_arg;
    struct packet_link_state state;
    struct ll_entry* pending;
    int states, received, stopped, destroyed, in_send, close_state, close_rx, send_state, enabled;
};

static struct ll_entry* packet(void) {
    struct ll_entry* p = ll_alloc_lldgram(8); assert(p);
    p->len = 8; p->dgram[0] = 42; return p;
}
static void free_packet(struct ll_entry* p) { queue_dgram_free(p); queue_entry_free(p); }
static void publish(struct fixture* f, int ready) {
    const struct packet_link_state state = {PACKET_LINK_OPEN, 1500, ready, 0};
    f->state = state; f->events.state_changed(f->event_arg);
}
static int accept_packet(void* arg, struct ll_entry* p) {
    struct fixture* f = arg; assert(!f->pending && !f->stopped);
    f->in_send = 1; f->pending = p; publish(f, 0); f->in_send = 0;
    return PACKET_LINK_OK;
}
static void enable(void* arg, int enabled) { ((struct fixture*)arg)->enabled = enabled; }
static void stop(void* arg) {
    struct fixture* f = arg; assert(!f->stopped); f->stopped = 1;
    if (f->pending) { free_packet(f->pending); f->pending = NULL; }
}
static void destroy(void* arg) {
    struct fixture* f = arg; assert(f->stopped && !f->destroyed); f->destroyed = 1;
}
static void changed(struct packet_link* link, void* arg) {
    struct fixture* f = arg; assert(!f->in_send && !f->stopped); f->states++;
    if (f->close_state) { packet_link_close(link); return; }
    if (f->send_state && packet_link_get_state(link).tx_ready) {
        f->send_state = 0; assert(packet_link_send(link, packet()) == PACKET_LINK_OK);
    }
}
static void received(struct packet_link* link, struct ll_entry* p, void* arg) {
    struct fixture* f = arg; assert(f->enabled && !f->in_send && !f->stopped);
    assert(p->dgram[0] == 42); f->received++; free_packet(p);
    if (f->close_rx) packet_link_close(link);
}
static int bind_driver(void* arg, const struct packet_link_events* events, void* event_arg) {
    struct fixture* f = arg;
    if (f->event_arg) return -1;
    f->events = *events; f->event_arg = event_arg; return 0;
}
static struct packet_link_state get_state(void* arg) { return ((struct fixture*)arg)->state; }
static int emit(struct fixture* f, struct ll_entry* p) {
    if (!f->enabled) return PACKET_LINK_WOULD_BLOCK;
    f->events.received(f->event_arg, p); return PACKET_LINK_OK;
}
static void create(struct fixture* f, struct UASYNC* ua) {
    char name[] = "test";
    struct packet_link_driver driver = {f, ua, name, bind_driver, get_state, accept_packet, enable, stop, destroy, PACKET_LINK_OUTGOING};
    const struct packet_link_callbacks callbacks = {changed, received};
    const struct packet_link_state initial = {PACKET_LINK_OPEN, 1500, 1, 0};
    f->state = initial;
    f->link = packet_link_bind(&driver, &callbacks, f); assert(f->link);
    driver.direction = PACKET_LINK_DIRECTION_UNKNOWN; name[0] = 'X';
    assert(!strcmp(packet_link_name(f->link), "test"));
    assert(packet_link_get_direction(f->link) == PACKET_LINK_OUTGOING);
    assert(!f->states && !f->enabled);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    size_t baseline = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct fixture f = {0}; create(&f, ua);
    struct ll_entry* p = packet();
    assert(emit(&f, p) == PACKET_LINK_WOULD_BLOCK);
    packet_link_set_rx_enabled(f.link, 1);
    assert(emit(&f, p) == PACKET_LINK_OK && f.received == 1);
    packet_link_set_rx_enabled(f.link, 0);
    p = packet(); assert(emit(&f, p) == PACKET_LINK_WOULD_BLOCK); free_packet(p);
    p = ll_alloc_lldgram(1501); assert(p); p->len = 1501;
    assert(packet_link_send(f.link, p) == PACKET_LINK_TOO_LARGE); free_packet(p);
    p = packet(); assert(packet_link_send(f.link, p) == PACKET_LINK_OK && f.pending == p);
    assert(!packet_link_get_state(f.link).tx_ready && !f.states);
    p = packet(); assert(packet_link_send(f.link, p) == PACKET_LINK_WOULD_BLOCK && p->dgram[0] == 42);
    uasync_drain_immediate(ua); assert(f.states == 1);
    free_packet(f.pending); f.pending = NULL; publish(&f, 1);
    assert(packet_link_send(f.link, p) == PACKET_LINK_OK);
    uasync_drain_immediate(ua); assert(f.states == 2);
    packet_link_close(f.link); assert(f.stopped && !f.destroyed);
    uasync_drain_immediate(ua); assert(f.destroyed && f.states == 2);

    struct fixture cancel = {0}; create(&cancel, ua); packet_link_close(cancel.link);
    uasync_drain_immediate(ua); assert(!cancel.states && cancel.destroyed);
    struct fixture state = {.close_state = 1}; create(&state, ua);
    uasync_drain_immediate(ua); assert(state.states == 1 && state.destroyed);
    struct fixture rx = {.close_rx = 1}; create(&rx, ua); packet_link_set_rx_enabled(rx.link, 1);
    assert(emit(&rx, packet()) == PACKET_LINK_OK);
    assert(rx.stopped && !rx.destroyed); uasync_drain_immediate(ua); assert(rx.destroyed && !rx.states);
    struct fixture nested = {.send_state = 1}; create(&nested, ua);
    uasync_drain_immediate(ua); assert(nested.states == 2 && nested.pending);
    struct packet_link_state closed = {PACKET_LINK_TERMINATED, 1500, 0, PACKET_LINK_ERROR};
    nested.state = closed; nested.events.state_changed(nested.event_arg);
    p = packet(); assert(packet_link_send(nested.link, p) == PACKET_LINK_CLOSED); free_packet(p);
    packet_link_close(nested.link); uasync_drain_immediate(ua); assert(nested.destroyed);
    uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    puts("PASS packet_link: ownership, TX blocked/resume, deferred/coalesced state, RX pause, close in callbacks, no leaks");
    return 0;
}
