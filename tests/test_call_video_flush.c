/* Настоящие transport/fragment/uasync, управляемый router без сетевых таймеров. */
#define etcp_router_conn_get_key test_router_conn_get
#define etcp_router_conn_send test_router_conn_send
#define etcp_router_tx_stats_key test_router_tx_stats
#define etcp_router_conn_send_q_has_room test_router_send_q_has_room
#define etcp_router_conn_resume_receive test_router_resume_receive
#define etcp_router_conn_on_send_ready test_router_on_send_ready
#define etcp_router_cancel_send_ready test_router_cancel_send_ready
#include "../src/call/call_video_transport.c"
#undef etcp_router_conn_get_key
#undef etcp_router_conn_send
#undef etcp_router_tx_stats_key
#undef etcp_router_conn_send_q_has_room
#undef etcp_router_conn_resume_receive
#undef etcp_router_conn_on_send_ready
#undef etcp_router_cancel_send_ready
#include <assert.h>
#include <stdio.h>

/* Один batch, без poll/timeouts: тест не может случайно продолжить TX таймером 50мс. */
extern void process_immediate_queue(struct UASYNC *ua);

static struct ETCP_ROUTER_CONN router, control_router;
static unsigned packets, received, room, resumed, reject_send, control_attempts;
static unsigned frame_offset, frames_sent;
static uint64_t debt;
static queue_threshold_callback_fn ready_callback;
static void *ready_arg;
static int encoder_admitted;
enum { FRAME_BYTES = 45001 };
static uint8_t output[FRAME_BYTES];
static void encoder_writable(void *arg, int admit) { (void)arg; encoder_admitted = admit; }

struct ETCP_ROUTER_CONN *test_router_conn_get(struct UTUN_INSTANCE *inst, uint64_t group, const uint8_t peer[32], uint8_t svc) {
    (void)inst; (void)group; (void)peer;
    if (svc == ETCP_RT_ID_CALL_VIDEO_CONTROL) return &control_router;
    assert(svc == ETCP_RT_ID_CALL_VIDEO); return &router;
}
int test_router_conn_send(struct ETCP_ROUTER_CONN *conn, const uint8_t *data, size_t len, int mode) {
    assert(mode == (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    if (conn == &control_router) {
        assert(len == 17 && data[0] == VIDEO_KEY_REQUEST);
        control_attempts++; return reject_send ? -1 : 0;
    }
    assert(conn == &router);
    struct call_video_frame f;
    assert(!call_video_fragment_parse(data, len, &f));
    assert(f.call_id == 7 && f.frame_id == frames_sent + 1 && f.offset == frame_offset);
    if (reject_send) return -1;
    if (f.frame_id == 1) {
        assert(f.size == FRAME_BYTES && received + f.length <= sizeof(output));
        memcpy(output + received, f.data, f.length); received += f.length;
    }
    for (unsigned i = 0; i < f.length; ++i) {
        unsigned offset = f.offset + i;
        assert(f.data[i] == (uint8_t)(offset * 17 + offset / 257));
    }
    frame_offset += f.length;
    if (frame_offset == f.size) { frame_offset = 0; frames_sent++; }
    router.tx_seq++; packets++; return 0;
}
int test_router_tx_stats(struct UTUN_INSTANCE *inst, uint64_t group, const uint8_t peer[32], uint8_t svc,
                        uint64_t now_us, struct ETCP_ROUTER_TX_STATS *out) {
    (void)inst; (void)group; (void)peer; (void)svc; (void)now_us;
    *out = (struct ETCP_ROUTER_TX_STATS){.local_epoch = router.reset_id, .peer_epoch = router.peer_reset_id,
        .ack_next_seq = router.tx_acked, .inflight_bytes = debt, .ready = router.peer_reset_id != 0};
    return router.reset_id ? 0 : 1;
}
int test_router_send_q_has_room(const struct ETCP_ROUTER_CONN *conn) {
    assert(conn == &router); return room;
}
void test_router_resume_receive(struct ETCP_ROUTER_CONN *conn) {
    (void)conn; resumed++;
}
void test_router_on_send_ready(struct ETCP_ROUTER_CONN *conn, struct queue_waiter_handle *h,
    queue_threshold_callback_fn callback, void *arg) {
    assert(conn == &router); (void)h; ready_callback = callback; ready_arg = arg;
}
void test_router_cancel_send_ready(struct queue_waiter_handle *h) {
    (void)h; ready_callback = NULL; ready_arg = NULL;
}

static void queue_frame(struct call_video_transport *v, unsigned size, unsigned fps) {
    struct video_tx_frame *tail = &v->tx[v->tx_count++]; assert(v->tx_count <= VIDEO_TX_FRAMES);
    uint64_t id = ++v->tx_last_id;
    v->tx_bytes += size; v->tx_last_pts = 1000000 + (id - 1) * 33333;
    tail->data = u_malloc(size); assert(tail->data);
    for (unsigned i = 0; i < size; i++) tail->data[i] = (uint8_t)(i * 17 + i / 257);
    tail->started_tb = get_time_tb();
    tail->frame = (struct call_video_frame){.call_id = 7, .frame_id = id, .pts_us = v->tx_last_pts,
        .format = {1, 640, 480, fps, 0, CALL_VIDEO_H264, 31, 4000000, CALL_VIDEO_BASELINE},
        .size = size, .flags = id == 1 ? CALL_VIDEO_KEY : 0};
}
static struct call_video_transport *setup(struct call_session *s) {
    struct call_video_transport *v = u_calloc(1, sizeof(*v)); assert(v);
    v->session = s; s->video = v;
    v->admit = 1; v->ops.writable = encoder_writable;
    v->receiver = call_video_receiver_create(); assert(v->receiver);
    v->have_peer = 1; v->local_revision = v->acknowledged_revision = 1;
    v->local.encode[0] = (struct call_video_limit){1280, 720, 60, 1280 * 720 * 60, 4000000, CALL_VIDEO_BASELINE, 31};
    v->peer.decode[0] = v->local.encode[0];
    queue_frame(v, FRAME_BYTES, 30);
    router.reset_id = 10; router.peer_reset_id = 20; router.tx_seq = router.tx_acked = 0;
    packets = received = resumed = reject_send = frames_sent = frame_offset = 0; room = 1; debt = 0;
    memset(output, 0, sizeof(output)); return v;
}
static void check_complete(struct call_video_transport *v) {
    assert(packets == 38 && received == FRAME_BYTES && !v->tx_count && !v->tx_bytes && !v->flush_pending);
    assert(v->ack_count == 1 && v->awaiting_ack[v->ack_head].size == FRAME_BYTES && buffered_video_us(v) == 33333);
    for (unsigned i = 0; i < FRAME_BYTES; i++) assert(output[i] == (uint8_t)(i * 17 + i / 257));
}
static void cleanup(struct call_session *s) {
    call_video_transport_stop(s); assert(!s->video && resumed == 1);
    unsigned before = packets;
    uasync_drain_immediate(s->ctx->inst->ua);
    assert(packets == before);
    assert(s->ctx->inst->ua->timer_alloc_count == s->ctx->inst->ua->timer_free_count);
}
int main(void) {
    debug_set_level(DEBUG_LEVEL_WARN);
    struct UASYNC *ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE inst = {.ua = ua};
    struct call_ctx ctx = {.inst = &inst};
    struct call_session s = {.ctx = &ctx, .call_id = 7};
    router.send_q = queue_new(ua, 0, 0, 0, "video_flush_test"); assert(router.send_q);

    struct call_video_transport *v = setup(&s);
    flush(v); assert(packets == 16 && v->flush_pending);
    process_immediate_queue(ua); assert(packets == 32 && v->flush_pending);
    process_immediate_queue(ua); check_complete(v); cleanup(&s);
    puts("PASS: 45001-byte frame completes in bounded batches without audio or periodic timers; bytes preserved");

    for (unsigned gate = 0; gate < 2; gate++) {
        v = setup(&s); flush(v);
        if (gate) debt = 1000000; else room = 0;
        process_immediate_queue(ua); assert(packets == 16 && !v->flush_pending && v->flush_net_wait == 1);
        assert(!encoder_admitted);
        process_immediate_queue(ua); assert(packets == 16);
        room = 1; debt = 0;
        if (!gate) { assert(ready_callback && ready_arg == v); ready_callback(router.send_q, ready_arg); }
        else flush(v);
        uasync_drain_immediate(ua); check_complete(v); assert(encoder_admitted); cleanup(&s);
    }
    puts("PASS: router room and inflight debt stop continuation without spinning; TX resumes exactly once");

    v = setup(&s); flush(v);
    v->tx[0].started_tb -= 30001; reject_send = 1;
    process_immediate_queue(ua);
    assert(!v->flush_pending && !v->need_key && v->tx_count == 1 && v->tx[0].frame.offset == received && packets == 16);
    reject_send = 0; flush(v); uasync_drain_immediate(ua); check_complete(v); cleanup(&s);
    puts("PASS: a partial frame survives a long stall and router rejection; its tail resumes at the same offset");

    v = setup(&s); control_attempts = 0; reject_send = 1;
    v->key_request_pending = 1; send_key_request(v); send_key_request(v);
    assert(v->key_request_pending && !v->key_requested && control_attempts == 1);
    reject_send = 0; v->key_request_tb -= 5001; send_key_request(v); send_key_request(v);
    assert(!v->key_request_pending && v->key_requested && control_attempts == 2);
    cleanup(&s);
    puts("PASS: a rejected KEY request is retained, rate-limited and retried");

    v = setup(&s); room = 0;
    v->tx_last_pts += VIDEO_TX_SPAN_US; writable(v); assert(!encoder_admitted);
    v->tx_last_pts -= VIDEO_TX_SPAN_US;
    flush(v); v->peer.decode[0].maximum_bps = 3000000;
    flush(v); assert(!packets && !v->tx_count && v->need_key); cleanup(&s);

    v = setup(&s); v->acknowledged_revision = 0;
    flush(v); assert(!packets && !v->flush_pending);
    v->acknowledged_revision = 1; flush(v); uasync_drain_immediate(ua); check_complete(v); cleanup(&s);
    puts("PASS: FIFO bound and CAPS withdrawal preserve transport admission rules; no audio needed");

    v = setup(&s); flush(v);
    flush(v); assert(packets == 32);
    process_immediate_queue(ua); check_complete(v); cleanup(&s);
    puts("PASS: a new flush supersedes pending continuation without duplicate fragments");

    v = setup(&s); queue_frame(v, 1324, 30); queue_frame(v, 77, 60);
    router.peer_reset_id = 0; router.tx_seq = router.tx_acked = UINT32_MAX - 38;
    flush(v);
    assert(v->tx_count == 3 && !v->ack_count && unsent_bytes(v) == FRAME_BYTES + 1324 + 77 - received);
    assert(buffered_video_us(v) == 83332);
    router.peer_reset_id = 20; uasync_drain_immediate(ua);
    assert(!v->tx_count && v->ack_count == 3 && frames_sent == 3 && buffered_video_us(v) == 83332);
    assert(v->awaiting_ack[(v->ack_head + 1) % VIDEO_ACK_FRAMES].size == 1324);
    struct ETCP_ROUTER_TX_STATS net;
    router.tx_acked = UINT32_MAX - 1; assert(!router_tx_snapshot(v, get_time_us(), &net));
    assert(v->ack_count == 3);
    router.tx_acked = 1; assert(!router_tx_snapshot(v, get_time_us(), &net));
    assert(v->ack_count == 1 && v->awaiting_ack[v->ack_head].frame_id == 3
        && v->awaiting_ack[v->ack_head].size == 77 && buffered_video_us(v) == 16666);
    assert(!router_tx_snapshot(v, get_time_us(), &net) && v->ack_count == 1);
    router.tx_acked = router.tx_seq; assert(!router_tx_snapshot(v, get_time_us(), &net));
    assert(!v->ack_count && !buffered_video_us(v)); cleanup(&s);
    puts("PASS: exact sizes, first handshake and cumulative ACK across sequence wrap; partial frame counted once");

    v = setup(&s); queue_frame(v, 20000, 30);
    flush(v); uasync_drain_immediate(ua);
    assert(v->ack_count == 2);
    router.peer_reset_id++; router.tx_seq = router.tx_acked = 0;
    assert(!router_tx_snapshot(v, get_time_us(), &net) && !v->ack_count && v->need_key); cleanup(&s);
    puts("PASS: a new router epoch clears unconfirmed frames and starts recovery");

    v = setup(&s); flush(v); cleanup(&s); assert(packets == 16);
    v = setup(&s); flush(v); discard_tx(v, "test_detach");
    assert(!v->tx_count && !v->flush_pending && v->need_key);
    uasync_drain_immediate(ua); assert(packets == 16); cleanup(&s);
    puts("PASS: stop and discard cancel pending callbacks before freeing their owner/data");
    queue_free(router.send_q); uasync_destroy(ua, 0); return 0;
}
