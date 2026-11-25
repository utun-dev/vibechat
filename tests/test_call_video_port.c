/* Реальные mailbox/mutex/post: частичная передача, повторная занятость, конкурентный producer и отмена. */
#define call_video_send_frame test_video_send
#define call_video_configure test_video_configure
#define call_session_find test_session_find
#define chat_setting_get test_setting_get
#include "../src/call/call_video_port.c"
#undef call_video_send_frame
#undef call_video_configure
#undef call_session_find
#undef chat_setting_get
#include <assert.h>

static struct call_session session;
static const struct call_video_ops *callbacks;
static void *callback_arg;
static unsigned accepted, allowance, offered;
static int concurrent;
static unsigned woken;
static void wake(void *arg) { assert(arg == &woken); ++woken; }
static struct call_video_frame extra;

struct call_session *test_session_find(struct call_ctx *ctx, uint64_t id) {
    return ctx == session.ctx && id == session.call_id ? &session : NULL;
}
const char *test_setting_get(struct UTUN_INSTANCE *inst, const char *name) {
    (void)inst; (void)name; return "300:4000";
}
int test_video_configure(struct UTUN_INSTANCE *inst, uint64_t id, const struct call_video_caps *caps,
    const struct call_video_ops *ops, void *arg) {
    (void)inst; (void)id; (void)caps; callbacks = ops; callback_arg = arg; return 0;
}
int test_video_send(struct UTUN_INSTANCE *inst, const struct call_video_frame *f) {
    (void)inst; offered++;
    if (concurrent) { assert(call_video_port_send(&extra, 1) == 0); concurrent = 0; }
    if (accepted == allowance) return 0;
    assert(f->frame_id == accepted + 1 && f->pts_us == (int64_t)f->frame_id * 33333);
    assert(f->size == 16);
    for (unsigned i = 0; i < f->size; ++i) assert(f->data[i] == f->frame_id);
    accepted++; return 1;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    struct UASYNC *ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE inst = {.ua = ua}; struct call_ctx ctx = {.inst = &inst}; inst.call = &ctx;
    session = (struct call_session){.ctx = &ctx, .call_id = 7, .state = CALL_ST_ACTIVE};
    assert(!call_video_port_attach(&inst));
    struct call_video_caps caps = {0};
    assert(!call_video_port_open(7, &caps)); uasync_poll(ua, 0);
    assert(call_video_port_set_wake(7, wake, &woken));
    assert(callbacks && callbacks->writable); callbacks->writable(callback_arg, 1);
    uint8_t data[8][16]; struct call_video_frame frames[8];
    for (unsigned i = 0; i < 8; ++i) {
        memset(data[i], i + 1, sizeof(data[i]));
        frames[i] = (struct call_video_frame){.call_id = 7, .frame_id = i + 1, .pts_us = (i + 1) * 33333,
            .format = {1, 640, 480, 30, 0, CALL_VIDEO_H264, 31, 4000000, CALL_VIDEO_BASELINE},
            .size = 16, .length = 16, .data = data[i]};
    }
    extra = frames[7]; concurrent = 1; allowance = 2;
    assert(call_video_port_send(frames, 8) == 1);
    memset(data, 0, sizeof(data)); // Порт обязан владеть собственной копией.
    uasync_poll(ua, 0);
    assert(accepted == 2 && port.tx_count == 6 && !port.tx_processing && !port.pending);
    unsigned before = offered; uasync_poll(ua, 0); assert(offered == before); // Нет busy loop.
    struct call_video_port_status status;
    assert(!call_video_port_status(7, &status) && !status.can_encode && !status.tx_idle && !status.key_serial);
    for (unsigned i = 0; i < 3; ++i) {
        callbacks->writable(callback_arg, 0); uasync_poll(ua, 0);
        assert(accepted == 2 && port.tx_count == 6 && !port.pending);
    }
    allowance = 8; callbacks->writable(callback_arg, 1); uasync_poll(ua, 0);
    assert(accepted == 8 && !port.tx_count);
    assert(!call_video_port_status(7, &status) && status.can_encode && status.tx_idle && !status.key_serial);
    callbacks->writable(callback_arg, 0);
    assert(!call_video_port_status(7, &status) && !status.can_encode && status.tx_idle);
    callbacks->writable(callback_arg, 1);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS video port: all 8 frames preserved across partial send and repeated busy; no KEY or spin");
    for (unsigned i = 0; i < 8; ++i) { assert(callbacks->receive_ready(callback_arg, 16)); callbacks->frame(callback_arg, &frames[i]); }
    assert(woken == 8 && !call_video_port_status(7, &status) && status.rx_frames == 8);
    // RX уже работает: добавление собственных encoder CAPS не сбрасывает очередь и callbacks.
    caps.decode[0] = (struct call_video_limit){1280, 720, 60, 1280 * 720 * 60, 4000000, 3, 42};
    callbacks->capabilities(callback_arg, &caps);
    caps.encode[0] = caps.decode[0];
    assert(!call_video_port_open(7, &caps)); uasync_poll(ua, 0);
    assert(!call_video_port_status(7, &status) && status.rx_frames == 8 && status.have_peer && status.can_encode);
    assert(status.peer.decode[0].fps == 60 && woken == 8 && port.rx_wake == wake && port.rx_wake_arg == &woken);
    assert(call_video_port_received(7) == 8 && port.local.encode[0].fps == 60);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS video port: same-call CAPS upgrade preserves RX frames, wake, peer and received count");
    pthread_mutex_lock(&lock); // Reader планировщика не должен захватывать mutex порта.
    assert(call_video_port_received(7) == 8 && !call_video_port_received(8));
    pthread_mutex_unlock(&lock);
    assert(!callbacks->receive_ready(callback_arg, 16));
    for (unsigned i = 0; i < 8; ++i) {
        struct call_video_frame f; assert(call_video_port_pull(7, &f) == 1 && f.frame_id == i + 1); u_free((void *)f.data);
        assert(callbacks->receive_ready(callback_arg, 16));
        assert(!call_video_port_status(7, &status) && status.rx_frames == 7 - i);
        assert(call_video_port_received(7) == 8);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS video port: exact RX depth, one wake per frame, resume as soon as space is available");
    assert(call_video_port_set_wake(7, NULL, NULL));
    call_video_port_close(7); uasync_poll(ua, 0);
    assert(!call_video_port_received(7));
    call_video_port_detach(&inst); uasync_poll(ua, 0);
    uasync_destroy(ua, 0); return 0;
}
