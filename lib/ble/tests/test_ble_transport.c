#include "ble_fixture_key.h"
/* Два/три настоящих common transport с управляемым асинхронным GATT backend: проверяется весь lifecycle. */
#include "ble_backend.h"
#include "u_async.h"
#include "mem.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #v); exit(1); } } while (0)
struct fake_link {
    ble_link_id id, other; struct ble_backend* peer; uint8_t data[512]; size_t len; int pending;
    enum ble_wire_channel channel; uint64_t ack_at, complete_at; uint16_t ack_id, ack_delivered; int ack_held, delivered;
};
struct ble_backend { struct ble_transport* owner; struct UASYNC* ua; void* task; int index, started; struct fake_link links[16]; };
static struct ble_backend* backends[3];
static unsigned int mtu = 23;
static int conservative_receiver;
static int hold, drop, reject_peer, ready_count, writable_count;
static int hold_ack;
static unsigned ack_delay_us;
static unsigned ack_timeouts;
static unsigned closed_count;
static unsigned drop_acks[3]; /* Потери ACK в эфире независимы от успешного локального completion. */
static uint64_t data_wire_bytes[3];
static ble_link_id incoming_ids[3];
static void run(void* arg);
static void schedule(struct ble_backend* b) { if (!b->task) b->task = uasync_call_soon(b->ua, b, run); CHECK(b->task); }
struct ble_backend* ble_backend_create(struct ble_transport* owner, struct UASYNC* ua, const struct ble_config* cfg) {
    struct ble_backend* b = u_calloc(1, sizeof(*b)); CHECK(b); b->owner = owner; b->ua = ua; b->index = (int)cfg->node_id - 1;
    backends[b->index] = b; schedule(b); return b;
}
void ble_backend_destroy(struct ble_backend* b) {
    if (b->task) uasync_call_soon_cancel(b->ua, b->task);
    backends[b->index] = NULL; u_free(b);
}
int ble_backend_advertise(struct ble_backend* b, const uint8_t* data, size_t len) { (void)b; (void)data; (void)len; return 0; }
static struct fake_link* slot(struct ble_backend* b, ble_link_id id) {
    for (int i = 0; i < 16; i++) if (b->links[i].id == id) return &b->links[i];
    return NULL;
}
int ble_backend_connect(struct ble_backend* b, ble_link_id id, const struct ble_endpoint* ep) {
    struct ble_backend* p = backends[atoi(ep->id)]; CHECK(p);
    struct fake_link *a = slot(b, 0), *c = slot(p, 0); CHECK(a && c);
    struct ble_endpoint remote = {0}; snprintf(remote.id, sizeof(remote.id), "%d", b->index);
    ble_link_id other = ble_backend_incoming(p->owner, &remote); CHECK(other);
    *a = (struct fake_link){.id=id, .other=other, .peer=p};
    *c = (struct fake_link){.id=other, .other=id, .peer=b};
    incoming_ids[p->index] = other;
    schedule(b); schedule(p); return 0;
}
void ble_backend_close(struct ble_backend* b, ble_link_id id) {
    struct fake_link* f = slot(b, id); if (f) memset(f, 0, sizeof(*f));
}
int ble_backend_send(struct ble_backend* b, ble_link_id id, enum ble_wire_channel channel, const uint8_t* data, size_t len) {
    struct fake_link* f = slot(b, id); CHECK(f && !f->pending && len <= sizeof(f->data));
    CHECK(len <= mtu - 3);
    if (channel == BLE_WIRE_DATA) data_wire_bytes[b->index] += len;
    if (channel == BLE_WIRE_ACK) {
        CHECK(len == 2);
        uint64_t now = get_time_us();
        CHECK(!f->ack_at || now - f->ack_at >= BLE_ACK_INTERVAL_US);
        f->ack_at = now; f->ack_id = (uint16_t)((data[0] << 8) | data[1]);
    }
    f->channel = channel; memcpy(f->data, data, len); f->len = len; f->pending = 1; f->delivered = 0;
    f->complete_at = channel == BLE_WIRE_ACK ? get_time_us() + ack_delay_us : 0;
    schedule(b); return 0;
}
static void run(void* arg) {
    struct ble_backend* b = arg; b->task = NULL;
    if (!b->started) { b->started = 1; ble_backend_status(b->owner, 1, "fake ready"); }
    for (int i = 0; i < 16; i++) {
        struct fake_link* f = &b->links[i]; if (!f->id) continue;
        if (!hold_ack && f->ack_held) {
            uint8_t ack[2] = {f->ack_id >> 8, f->ack_id}; f->ack_held = 0; f->ack_delivered = f->ack_id;
            if (f->peer && slot(f->peer, f->other)) ble_backend_received(f->peer->owner, f->other, BLE_WIRE_ACK, ack, 2);
        }
        if (!f->len) {
            int outgoing = 0; CHECK(!ble_get_outgoing(b->owner, f->id, &outgoing));
            if (outgoing && ble_backend_profile(b->owner, f->id, ble_backend_public_key(f->peer->owner), BLE_PUBLIC_KEY_SIZE, NULL, 0)) continue;
            f->len = 1; ble_backend_opened(b->owner, f->id, conservative_receiver && b->index == 1 ? 23 : mtu);
        }
        if (!f->pending || hold) continue;
        struct fake_link* target = f->peer ? slot(f->peer, f->other) : NULL;
        if (target && !target->len) { schedule(b); continue; } /* Удалённая подписка должна завершиться до доставки HELLO. */
        ble_link_id id = f->id, other = f->other;
        struct ble_backend* peer = f->peer;
        if (!f->delivered) {
            uint8_t data[512]; size_t len = f->len; memcpy(data, f->data, len); f->delivered = 1;
            int dropped = f->channel != BLE_WIRE_ACK && data[f->channel == BLE_WIRE_DATA ? 2 : 0] == 3 && drop && --drop == 0;
            if (f->channel == BLE_WIRE_ACK && drop_acks[b->index]) { drop_acks[b->index]--; dropped = 1; }
            if (hold_ack && f->channel == BLE_WIRE_ACK) f->ack_held = 1;
            else if (!dropped && peer && slot(peer, other)) {
                if (f->channel == BLE_WIRE_ACK) f->ack_delivered = f->ack_id;
                ble_backend_received(peer->owner, other, f->channel, data, len);
            }
        }
        if (slot(b, id)) {
            if (get_time_us() < f->complete_at) { schedule(b); continue; }
            f->pending = 0; ble_backend_sent(b->owner, id, 1);
        }
    }
}
static int validate(void* arg, ble_link_id id, uint64_t node) { (void)arg; (void)id; (void)node; return !reject_peer; }
static void connection(void* arg, ble_link_id id, uint64_t node, enum ble_state s, const char* reason) {
    (void)arg; (void)id; (void)node; if (s == BLE_READY) ready_count++;
    if (s == BLE_CLOSED) closed_count++;
    if (s == BLE_CLOSED && !strcmp(reason, "ACK progress timeout")) ack_timeouts++;
}
static void writable(void* arg, ble_link_id id) { (void)arg; (void)id; writable_count++; }
static void drain(struct UASYNC* ua) {
    uint64_t end = get_time_us() + 2 * BLE_ACK_INTERVAL_US;
    do { uasync_poll(ua, 10); } while (get_time_us() < end);
}
static struct ll_entry* frame(unsigned int size, uint8_t value) {
    struct ll_entry* e = ll_alloc_lldgram(size); CHECK(e); e->len = size; memset(e->dgram, value, size); return e;
}
static void release(struct ll_entry* e) { queue_dgram_free(e); queue_entry_free(e); }
struct resume_writer { struct ble_transport* radio; unsigned sent, wakes, goal; int fragmented; };
/* Следующий кадр подаёт только writable: ни polling tx_ready, ни пробный отказ не маскируют потерянное пробуждение. */
static void resume_write(void* arg, ble_link_id id) {
    struct resume_writer* w = arg; w->wakes++;
    CHECK(ble_tx_ready(w->radio, id));
    if (w->sent == w->goal) return;
    unsigned size = w->fragmented && w->sent % 2 ? 1500 : 7;
    CHECK(!ble_tx_put(w->radio, id, frame(size, w->sent))); w->sent++;
}
/* ACK до completion не освобождает занятую локальную очередь; обратный порядок ждёт ACK окна из одного кадра. */
static void exercise_resume_order(unsigned test_mtu, int ack_first) {
    mtu = test_mtu; hold = hold_ack = drop = reject_peer = 0;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct resume_writer writer = {0};
    struct ble_config cfg = {.node_id = 1, .tx_limit = 1, .tx_window = 1, .arg = &writer}; fixture_ble_config(&cfg);
    cfg.callbacks.writable = resume_write;
    struct ble_transport* a = ble_create(ua, &cfg); writer.radio = a;
    cfg.node_id = 2; fixture_ble_config(&cfg); cfg.arg = NULL; cfg.callbacks.writable = NULL;
    struct ble_transport* b = ble_create(ua, &cfg); CHECK(a && b && !ble_start(a) && !ble_start(b)); drain(ua);
    struct ble_endpoint ep = {"1"}; ble_link_id id = ble_connect(a, &ep, 2); CHECK(id); drain(ua);
    ble_link_id other = incoming_ids[1]; hold = hold_ack = 1;
    writer = (struct resume_writer){.radio = a, .goal = 8};
    CHECK(!ble_tx_put(a, id, frame(7, 0))); writer.sent = 1;
    for (unsigned i = 0; i < writer.goal; i++) {
        struct fake_link* f = slot(backends[0], id); CHECK(f && f->pending && f->channel == BLE_WIRE_DATA);
        uint8_t ack[2] = {f->data[0], f->data[1]};
        CHECK(!ble_tx_ready(a, id) && writer.wakes == i);
        ble_backend_received(b, other, f->channel, f->data, f->len);
        if (ack_first) ble_backend_received(a, id, BLE_WIRE_ACK, ack, sizeof(ack));
        else { f->pending = 0; ble_backend_sent(a, id, 1); }
        CHECK(!ble_tx_ready(a, id) && writer.wakes == i);
        if (ack_first) { f->pending = 0; ble_backend_sent(a, id, 1); }
        else ble_backend_received(a, id, BLE_WIRE_ACK, ack, sizeof(ack));
        CHECK(writer.wakes == i + 1);
        ble_backend_received(a, id, BLE_WIRE_ACK, ack, sizeof(ack)); CHECK(writer.wakes == i + 1);
        struct ll_entry* e = ble_rx_get(b, other); CHECK(e && e->len == 7 && e->dgram[0] == i); release(e);
    }
    CHECK(writer.sent == writer.goal && ble_tx_ready(a, id));
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS BLE resume order: MTU=%u ACK_first=%d wakes=%u frames=%u; no refused put/polling",
               test_mtu, ack_first, writer.wakes, writer.sent);
    ble_destroy(a); ble_destroy(b); uasync_destroy(ua, 0); hold = hold_ack = 0;
}
/* Production tx_limit=1/window=3: только callback подаёт следующие короткие и фрагментированные кадры. */
static void exercise_resume_stream(unsigned test_mtu) {
    mtu = test_mtu; hold = hold_ack = drop = reject_peer = 0;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct resume_writer writer = {0};
    struct ble_config cfg = {.node_id = 1, .tx_limit = 1, .tx_window = 3, .arg = &writer}; fixture_ble_config(&cfg);
    cfg.callbacks.writable = resume_write;
    struct ble_transport* a = ble_create(ua, &cfg); writer.radio = a;
    cfg.node_id = 2; fixture_ble_config(&cfg); cfg.arg = NULL; cfg.callbacks.writable = NULL;
    struct ble_transport* b = ble_create(ua, &cfg); CHECK(a && b && !ble_start(a) && !ble_start(b)); drain(ua);
    struct ble_endpoint ep = {"1"}; ble_link_id id = ble_connect(a, &ep, 2); CHECK(id); drain(ua);
    ble_link_id other = incoming_ids[1];
    writer = (struct resume_writer){.radio = a, .goal = 24, .fragmented = 1};
    CHECK(!ble_tx_put(a, id, frame(7, 0))); writer.sent = 1;
    uint64_t started = get_time_us(); unsigned received = 0;
    while ((received < writer.goal || writer.wakes < writer.goal) && get_time_us() - started < 3000000) {
        uasync_poll(ua, 10);
        struct ll_entry* e;
        while ((e = ble_rx_get(b, other))) {
            CHECK(e->len == (received % 2 ? 1500 : 7));
            for (unsigned i = 0; i < e->len; i++) CHECK(e->dgram[i] == received);
            received++; release(e);
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BLE resume stream: MTU=%u TX=%u RX=%u wakes=%u elapsed_us=%llu",
               test_mtu, writer.sent, received, writer.wakes, (unsigned long long)(get_time_us() - started));
    CHECK(writer.sent == writer.goal && received == writer.goal && writer.wakes == writer.goal);
    CHECK(ble_tx_ready(a, id));
    ble_destroy(a); ble_destroy(b); uasync_destroy(ua, 0);
}
/* Непрерывный RX при ACK completion >15ms не должен вытеснять встречный DATA. */
static void exercise_ack_fairness(void) {
    mtu = 517; hold = drop = reject_peer = hold_ack = 0; ack_delay_us = 0;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ble_config cfg = {.node_id = 1}; fixture_ble_config(&cfg); struct ble_transport* a = ble_create(ua, &cfg);
    cfg.node_id = 2; fixture_ble_config(&cfg); struct ble_transport* b = ble_create(ua, &cfg); CHECK(a && b && !ble_start(a) && !ble_start(b)); drain(ua);
    struct ble_endpoint ep = {"1"}; ble_link_id id = ble_connect(a, &ep, 2); CHECK(id); drain(ua);
    ble_link_id other = incoming_ids[1]; ack_delay_us = 20000;
    int queued = 0, replied = 0; unsigned received = 0;
    uint64_t started = get_time_us();
    while (!replied && get_time_us() - started < 250000) {
        while (ble_tx_ready(a, id)) CHECK(!ble_tx_put(a, id, frame(500, 0x31)));
        uasync_poll(ua, 0);
        struct fake_link* receiver = slot(backends[1], other); CHECK(receiver);
        if (!queued && receiver->pending && receiver->channel == BLE_WIRE_ACK) {
            CHECK(!ble_tx_put(b, other, frame(1, 0xa7))); queued = 1;
        }
        struct ll_entry* e;
        while ((e = ble_rx_get(b, other))) { received++; release(e); }
        while ((e = ble_rx_get(a, id))) { CHECK(e->len == 1 && e->dgram[0] == 0xa7); replied++; release(e); }
    }
    CHECK(queued && replied == 1 && received > 1);
    printf("BLE ACK fairness PASS (completion=20ms, continuous RX, DATA returned in %lluus)\n",
           (unsigned long long)(get_time_us() - started));
    ble_destroy(a); ble_destroy(b); uasync_destroy(ua, 0); ack_delay_us = 0;
}
static void exercise(unsigned int test_mtu) {
    mtu = test_mtu; hold = drop = reject_peer = ready_count = writable_count = 0;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ble_transport* t[3];
    for (int i = 0; i < 3; i++) {
        struct ble_config cfg = {.tx_window = BLE_QUEUE_MAX}; cfg.node_id = i + 1; fixture_ble_config(&cfg);
        cfg.callbacks.validate = validate; cfg.callbacks.connection = connection; cfg.callbacks.writable = writable;
        t[i] = ble_create(ua, &cfg); CHECK(t[i] && !ble_start(t[i]));
    }
    drain(ua);
    struct ble_endpoint ep = {"1"};
    ble_link_id a = ble_connect(t[0], &ep, 2); CHECK(a); drain(ua); CHECK(ready_count == 2);
    ble_link_id b = incoming_ids[1];
    CHECK(!ble_tx_put(t[0], a, frame(1500, 0x31))); CHECK(!ble_tx_put(t[1], b, frame(1499, 0x71))); drain(ua);
    struct ll_entry* e = ble_rx_get(t[1], b); CHECK(e && e->len == 1500);
    for (int i = 0; i < e->len; i++) CHECK(e->dgram[i] == 0x31);
    release(e);
    e = ble_rx_get(t[0], a); CHECK(e && e->len == 1499 && e->dgram[1498] == 0x71); release(e);
    e = frame(BLE_FRAME_MAX + 1, 0x81); CHECK(ble_tx_put(t[0], a, e) == BLE_ERROR); release(e);
    e = frame(BLE_INVITE_FRAME_MAX + 1, 0x82);
    CHECK(ble_channel_tx_put(t[0], a, BLE_CHANNEL_INVITE, e) == BLE_ERROR); release(e);
    CHECK(!ble_channel_tx_put(t[0], a, BLE_CHANNEL_INVITE, frame(BLE_INVITE_FRAME_MAX, 0x83))); drain(ua);
    e = ble_channel_rx_get(t[1], b, BLE_CHANNEL_INVITE); CHECK(e && e->len == BLE_INVITE_FRAME_MAX);
    for (int i = 0; i < e->len; i++) CHECK(e->dgram[i] == 0x83);
    release(e);
    /* Два независимых канала одновременно, включая отказ TX и потерю фрагмента только INVITE. */
    CHECK(ble_channel_tx_ready(t[0], a, BLE_CHANNEL_INVITE));
    CHECK(!ble_tx_put(t[0], a, frame(1500, 0x61)));
    CHECK(!ble_channel_tx_put(t[0], a, BLE_CHANNEL_INVITE, frame(1100, 0x62)));
    e = frame(1, 0x63);
    CHECK(ble_channel_tx_put(t[0], a, BLE_CHANNEL_INVITE, e) == BLE_WOULD_BLOCK); release(e);
    CHECK(!ble_channel_tx_put(t[1], b, BLE_CHANNEL_INVITE, frame(1100, 0x64)));
    drain(ua);
    e = ble_rx_get(t[1], b); CHECK(e && e->len == 1500 && e->dgram[1499] == 0x61); release(e);
    e = ble_channel_rx_get(t[1], b, BLE_CHANNEL_INVITE); CHECK(e && e->len == 1100 && e->dgram[1099] == 0x62); release(e);
    e = ble_channel_rx_get(t[0], a, BLE_CHANNEL_INVITE); CHECK(e && e->len == 1100 && e->dgram[1099] == 0x64); release(e);
    CHECK(!ble_rx_get(t[0], a) && !ble_rx_get(t[1], b));
    drop = 2; CHECK(!ble_channel_tx_put(t[0], a, BLE_CHANNEL_INVITE, frame(1100, 0x65))); drain(ua);
    CHECK(!ble_channel_rx_get(t[1], b, BLE_CHANNEL_INVITE));
    CHECK(!ble_channel_tx_put(t[0], a, BLE_CHANNEL_INVITE, frame(20, 0x66))); drain(ua);
    e = ble_channel_rx_get(t[1], b, BLE_CHANNEL_INVITE); CHECK(e && e->len == 20); release(e);
    drop = 2; CHECK(!ble_tx_put(t[0], a, frame(1500, 0x44))); drain(ua); CHECK(!ble_rx_get(t[1], b));
    CHECK(!ble_tx_put(t[0], a, frame(5, 0x55))); drain(ua); e = ble_rx_get(t[1], b); CHECK(e && e->len == 5); release(e);
    unsigned before_writable = writable_count;
    hold = 1;
    for (int i = 0; i < BLE_QUEUE_MAX; i++) CHECK(!ble_tx_put(t[0], a, frame(1, i)));
    e = frame(1, 99); CHECK(ble_tx_put(t[0], a, e) == BLE_WOULD_BLOCK); CHECK(!e->owner && e->dgram[0] == 99); release(e);
    hold = 0; schedule(backends[0]); drain(ua); CHECK(writable_count > before_writable);
    for (int i = 0; i < BLE_QUEUE_MAX; i++) { e = ble_rx_get(t[1], b); CHECK(e && e->dgram[0] == i); release(e); }
    CHECK(!ble_rx_get(t[1], b));
    for (int i = 0; i < BLE_QUEUE_MAX + 1; i++) { CHECK(!ble_tx_put(t[0], a, frame(2, i))); drain(ua); }
    for (int i = 0; i < BLE_QUEUE_MAX; i++) { e = ble_rx_get(t[1], b); CHECK(e && e->dgram[0] == i); release(e); }
    CHECK(!ble_rx_get(t[1], b));
    strcpy(ep.id, "2"); ble_link_id c = ble_connect(t[0], &ep, 3); CHECK(c); drain(ua); CHECK(ready_count == 4);
    hold = 1; CHECK(!ble_tx_put(t[0], a, frame(1500, 8))); CHECK(!ble_tx_put(t[0], c, frame(1500, 9)));
    ble_disconnect(t[0], a); hold = 0; schedule(backends[0]); drain(ua);
    e = ble_rx_get(t[2], incoming_ids[2]); CHECK(e && e->len == 1500 && e->dgram[1499] == 9); release(e);
    e = frame(1, 0); CHECK(ble_tx_put(t[0], a, e) == BLE_NOT_READY); release(e);
    /* Отказ политики на третьем независимом направлении не затрагивает существующее соединение. */
    reject_peer = 1; strcpy(ep.id, "2"); ble_link_id rejected = ble_connect(t[1], &ep, 3); CHECK(rejected); drain(ua);
    CHECK(ready_count == 4);
    e = frame(1, 0); CHECK(ble_tx_put(t[1], rejected, e) == BLE_NOT_READY); release(e);
    ble_set_incoming(t[2], 0);
    struct ble_endpoint denied = {"policy-test"};
    CHECK(!ble_backend_incoming(t[2], &denied));
    CHECK(!ble_rx_get(t[2], incoming_ids[2]));
    ble_set_incoming(t[2], 1);
    ble_link_id early = ble_backend_incoming(t[2], &denied); CHECK(early);
    uint8_t early_hello[19] = {0, 1, 1}; early_hello[10] = 1; early_hello[18] = 3;
    unsigned before_closed = closed_count;
    ble_backend_received(t[2], early, BLE_WIRE_DATA, early_hello, sizeof(early_hello));
    CHECK(closed_count == before_closed + 1 && ready_count == 4);
    for (int i = 0; i < 3; i++) ble_destroy(t[i]);
    drain(ua); uasync_destroy(ua, 0);
}
/* Локальный completion мгновенный, удалённые ACK задержаны: предел относится именно к очереди ниже backend. */
static void exercise_flow(void) {
    mtu = 517; hold = drop = reject_peer = hold_ack = 0;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ble_config cfg = {.node_id = 1, .tx_window = BLE_QUEUE_MAX}; fixture_ble_config(&cfg);
    cfg.callbacks.writable = writable; cfg.callbacks.connection = connection;
    struct ble_transport* a = ble_create(ua, &cfg); cfg.node_id = 2; fixture_ble_config(&cfg);
    struct ble_transport* b = ble_create(ua, &cfg); CHECK(a && b && !ble_start(a) && !ble_start(b)); drain(ua);
    struct ble_endpoint ep = {"1"}; ble_link_id id = ble_connect(a, &ep, 2); CHECK(id); drain(ua);
    ble_link_id other = incoming_ids[1];
    uint64_t base[2] = {data_wire_bytes[0], data_wire_bytes[1]};
    unsigned received[2] = {0}; hold_ack = 1;
    for (unsigned i = 0; i < BLE_QUEUE_MAX; i++) {
        CHECK(!ble_tx_put(a, id, frame(1500, i))); CHECK(!ble_tx_put(b, other, frame(1500, i)));
    }
    for (unsigned i = 0; i < 10000; i++) uasync_poll(ua, 0);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(data_wire_bytes[i] - base[i] <= BLE_TX_WINDOW_BYTES);
        CHECK(data_wire_bytes[i] - base[i] >= BLE_TX_WINDOW_BYTES - BLE_VALUE_MAX);
    }
    CHECK(!ble_tx_ready(a, id) && !ble_tx_ready(b, other));
    uint64_t paused = data_wire_bytes[0];
    struct fake_link* receiver = slot(backends[1], other); CHECK(receiver);
    uint8_t duplicate[2] = {receiver->ack_delivered >> 8, receiver->ack_delivered};
    /* Повтор уже доставленного ACK не подтверждает новые данные даже при зависшем ACK в backend. */
    ble_backend_received(a, id, BLE_WIRE_ACK, duplicate, 2);
    for (unsigned i = 0; i < 1000; i++) uasync_poll(ua, 0);
    CHECK(data_wire_bytes[0] == paused && !ble_tx_ready(a, id));
    /* Последний ACK потерян в обоих направлениях, а новый DATA уже не помещается в окно. */
    hold_ack = 0;
    slot(backends[0], id)->ack_held = receiver->ack_held = 0;
    drop_acks[0] = drop_acks[1] = 3;
    uint64_t recovery_started = get_time_us();
    schedule(backends[0]); schedule(backends[1]);
    uint64_t deadline = get_time_us() + 5000000;
    while ((received[0] < BLE_QUEUE_MAX || received[1] < BLE_QUEUE_MAX) && get_time_us() < deadline) {
        uasync_poll(ua, 10);
        for (unsigned i = 0; i < 2; i++) {
            struct ll_entry* e;
            while ((e = ble_rx_get(i ? b : a, i ? other : id))) {
                CHECK(e->len == 1500 && e->dgram[0] == received[i] && e->dgram[1499] == received[i]);
                received[i]++; release(e);
            }
        }
    }
    CHECK(received[0] == BLE_QUEUE_MAX && received[1] == BLE_QUEUE_MAX);
    CHECK(!drop_acks[0] && !drop_acks[1] && get_time_us() - recovery_started >= 3 * BLE_ACK_REPEAT_US);
    printf("BLE lost ACK recovery PASS (3 losses per direction, both windows full, elapsed_us=%llu)\n",
           (unsigned long long)(get_time_us() - recovery_started));
    drain(ua);
    /* Больше полного оборота wire ID; новые данные действительно приходят, старые ACK не открывают окно. */
    unsigned sent = 0, delivered = 0; deadline = get_time_us() + 30000000;
    while (delivered < 70000 && get_time_us() < deadline) {
        while (sent < 70000 && ble_tx_ready(a, id)) { CHECK(!ble_tx_put(a, id, frame(1, sent))); sent++; }
        uasync_poll(ua, 0);
        struct ll_entry* e;
        while ((e = ble_rx_get(b, other))) { CHECK(e->len == 1 && e->dgram[0] == (uint8_t)delivered); delivered++; release(e); }
    }
    CHECK(sent == 70000 && delivered == sent); drain(ua);
    struct fake_link* sender = slot(backends[0], id); CHECK(sender);
    uint8_t future[2] = {(uint8_t)((receiver->ack_id + 1) >> 8), (uint8_t)(receiver->ack_id + 1)};
    ble_backend_received(a, id, BLE_WIRE_ACK, future, 2); CHECK(!ble_tx_ready(a, id));
    /* Локальный send завершается, но peer исчез: независимый от pending watchdog закрывает линк через 15s. */
    ack_timeouts = 0;
    uint64_t started = get_time_us(); CHECK(!ble_tx_put(b, other, frame(1, 0)));
    while (!ack_timeouts && get_time_us() - started < 17000000) uasync_poll(ua, 1000);
    CHECK(ack_timeouts == 1 && get_time_us() - started >= 15000000 && !ble_tx_ready(b, other));
    ble_destroy(a); ble_destroy(b); uasync_destroy(ua, 0);
    puts("BLE flow PASS (12500-byte duplex bound, ACK >=15ms, duplicate ACK, resume, 70000 IDs, invalid ACK, timeout, teardown)");
}
/* Локальное завершение и ACK середины кадра не освобождают окно; ACK конца освобождает ровно один слот. */
static void exercise_frame_window(unsigned test_mtu) {
    mtu = test_mtu; hold = drop = reject_peer = hold_ack = 0;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ble_config cfg = {.node_id = 1, .tx_window = 2}; fixture_ble_config(&cfg); cfg.callbacks.writable = writable;
    struct ble_transport* a = ble_create(ua, &cfg); cfg.node_id = 2; fixture_ble_config(&cfg);
    struct ble_transport* b = ble_create(ua, &cfg); CHECK(a && b && !ble_start(a) && !ble_start(b)); drain(ua);
    struct ble_endpoint ep = {"1"}; ble_link_id id = ble_connect(a, &ep, 2); CHECK(id); drain(ua);
    ble_link_id other = incoming_ids[1]; hold_ack = 1;
    CHECK(!ble_tx_put(a, id, frame(1500, 0x41))); drain(ua);
    struct fake_link* receiver = slot(backends[1], other); CHECK(receiver && receiver->ack_held);
    uint16_t first_end = receiver->ack_id, partial = first_end - 1;
    uint8_t ack[2] = {partial >> 8, partial};
    ble_backend_received(a, id, BLE_WIRE_ACK, ack, sizeof(ack));
    CHECK(!ble_tx_put(a, id, frame(17, 0x42))); drain(ua);
    CHECK(!ble_tx_ready(a, id));
    struct ll_entry* e = frame(7, 0x43);
    CHECK(ble_tx_put(a, id, e) == BLE_WOULD_BLOCK && !e->owner);
    unsigned before_writable = writable_count;
    ack[0] = first_end >> 8; ack[1] = first_end;
    ble_backend_received(a, id, BLE_WIRE_ACK, ack, sizeof(ack));
    CHECK(ble_tx_ready(a, id) && writable_count > before_writable);
    CHECK(!ble_tx_put(a, id, e) && !ble_tx_ready(a, id));
    ble_backend_received(a, id, BLE_WIRE_ACK, ack, sizeof(ack)); CHECK(!ble_tx_ready(a, id));
    CHECK(!ble_channel_tx_put(a, id, BLE_CHANNEL_INVITE, frame(3, 0x44))); drain(ua);
    e = ble_channel_rx_get(b, other, BLE_CHANNEL_INVITE); CHECK(e && e->len == 3); release(e);
    hold_ack = 0; schedule(backends[1]); drain(ua);
    unsigned sizes[] = {1500, 17, 7};
    for (unsigned i = 0; i < 3; i++) {
        e = ble_rx_get(b, other); CHECK(e && e->len == sizes[i] && e->dgram[0] == 0x41 + i); release(e);
    }
    CHECK(!ble_rx_get(b, other) && ble_tx_ready(a, id));
    ble_destroy(a); ble_destroy(b); uasync_destroy(ua, 0);
    printf("BLE frame window PASS (MTU=%u, two frames, partial/final/duplicate ACK, writable, independent INVITE)\n", test_mtu);
}
/* ACL уже имеет MTU517, но новый Android GATT server ещё не получил onMtuChanged. RX не ограничивается TX fallback. */
static void exercise_unknown_mtu(void) {
    mtu = 517; conservative_receiver = 1; hold = hold_ack = drop = reject_peer = 0;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ble_config cfg = {.node_id = 1}; fixture_ble_config(&cfg);
    struct ble_transport* a = ble_create(ua, &cfg); cfg.node_id = 2; fixture_ble_config(&cfg);
    struct ble_transport* b = ble_create(ua, &cfg); CHECK(a && b && !ble_start(a) && !ble_start(b)); drain(ua);
    struct ble_endpoint ep = {"1"}; ble_link_id id = ble_connect(a, &ep, 2); CHECK(id); drain(ua);
    ble_link_id other = incoming_ids[1];
    CHECK(!ble_tx_put(a, id, frame(BLE_FRAME_MAX, 0x91)));
    CHECK(!ble_channel_tx_put(a, id, BLE_CHANNEL_INVITE, frame(BLE_INVITE_FRAME_MAX, 0x92)));
    CHECK(!ble_tx_put(b, other, frame(BLE_FRAME_MAX, 0x93))); drain(ua);
    struct ll_entry* e = ble_rx_get(b, other); CHECK(e && e->len == BLE_FRAME_MAX && e->dgram[1499] == 0x91); release(e);
    e = ble_channel_rx_get(b, other, BLE_CHANNEL_INVITE);
    CHECK(e && e->len == BLE_INVITE_FRAME_MAX && e->dgram[BLE_INVITE_FRAME_MAX - 1] == 0x92); release(e);
    e = ble_rx_get(a, id); CHECK(e && e->len == BLE_FRAME_MAX && e->dgram[1499] == 0x93); release(e);
    ble_destroy(a); ble_destroy(b); uasync_destroy(ua, 0); conservative_receiver = 0;
    puts("BLE unknown MTU PASS (TX minimum23, RX517, duplex DATA and full INVITE)");
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    size_t before = u_get_allocated_count();
    exercise_unknown_mtu(); CHECK(u_get_allocated_count() == before);
    for (unsigned i = 0; i < 2; i++) {
        exercise_resume_order(i ? 517 : 23, 0); exercise_resume_order(i ? 517 : 23, 1);
        exercise_resume_stream(i ? 517 : 23); CHECK(u_get_allocated_count() == before);
    }
    exercise_ack_fairness(); CHECK(u_get_allocated_count() == before);
    exercise(23); exercise(247); exercise(517);
    exercise_frame_window(23); exercise_frame_window(517);
    exercise_flow();
    CHECK(u_get_allocated_count() == before); puts("BLE transport tests PASS (MTU 23/247/517, duplex, loss, backpressure, peers, teardown)"); return 0;
}

int ble_backend_set_name(struct ble_backend* b, const char* name) { (void)b; (void)name; return BLE_OK; }
