/* Wire: границы размера пачек, полный разбор и атомарный отказ. */
#include "../src/call/call_audio_batch.h"
#include "../src/routing_layer/etcp_router.h"
#include "../lib/debug_config.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio batch test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

static void wire_test(void) {
    uint8_t opus[57], wire[CALL_MEDIA_MAX_BYTES], saved[CALL_MEDIA_MAX_BYTES];
    memset(opus, 0xa5, sizeof(opus));
    struct call_audio_batch_view b = {.header = {.subcmd = CALL_SUBCMD_MEDIA, .call_id = 1, .track_id = CALL_TRACK_AUDIO,
        .capture_epoch = 17, .codec_delay_samples = 312, .sent_ms = UINT32_MAX}};
    size_t length = 0;
    for (unsigned n = 1; n <= 10; n++) {
        b.header.count = n;
        b.frames[n - 1] = (struct call_audio_frame_view){.header = {(uint16_t)(65532 + n), 1000000 + n * 20000, 37}, .opus = opus};
        CHECK(call_audio_batch_encode(&b, wire, sizeof(wire), &length) == 0 && length == 27 + n * 49);
        struct call_audio_batch_view out;
        CHECK(call_audio_batch_parse(wire, length, &out) == 0 && out.header.count == n && out.header.sent_ms == UINT32_MAX);
        for (unsigned i = 0; i < n; i++) CHECK(out.frames[i].header.seq == b.frames[i].header.seq
            && out.frames[i].header.capture_pts_us == b.frames[i].header.capture_pts_us
            && out.frames[i].header.length == 37 && !memcmp(out.frames[i].opus, opus, 37));
    }
    struct call_audio_batch_view out, before;
    memset(&out, 0x77, sizeof(out)); before = out;
    for (size_t n = 0; n < length; n++) {
        CHECK(call_audio_batch_parse(wire, n, &out) == -1 && !memcmp(&out, &before, sizeof(out)));
    }
    CHECK(call_audio_batch_parse(wire, length + 1, &out) == -1);
    memcpy(saved, wire, sizeof(wire));
    wire[CALL_MEDIA_HDR_SIZE - 1] = 11;
    CHECK(call_audio_batch_parse(wire, length, &out) == -1);
    memcpy(wire, saved, sizeof(wire));
    struct call_pkt_audio_frame invalid = {1, 0, UINT16_MAX};
    memcpy(wire + CALL_MEDIA_HDR_SIZE, &invalid, sizeof(invalid));
    CHECK(call_audio_batch_parse(wire, length, &out) == -1);
    memset(wire, 0x55, sizeof(wire)); memcpy(saved, wire, sizeof(wire));
    CHECK(call_audio_batch_encode(&b, wire, length - 1, &length) == -1 && !memcmp(wire, saved, sizeof(wire)));
    b.header.count = 1; b.header.capture_epoch = 0;
    CHECK(call_audio_batch_encode(&b, wire, sizeof(wire), &length) == -1);
    b.frames[0].header.capture_pts_us = INT64_MIN;
    CHECK(call_audio_batch_encode(&b, wire, sizeof(wire), &length) == 0);
    CHECK(call_audio_batch_parse(wire, length, &out) == 0 && out.frames[0].header.capture_pts_us == INT64_MIN);
}

static void step(struct call_audio_batch_control *c, struct ETCP_ROUTER_TX_STATS *n, uint64_t t) {
    n->last_ack_us = t; ++n->acked_packets; CHECK(call_audio_batch_step(c, t, n, NULL) == 0);
}
static void control_test(void) {
    struct call_audio_batch_control c;
    struct ETCP_ROUTER_TX_STATS n = {.local_epoch = 1, .peer_epoch = 2, .ready = 1, .acked_packets = 1,
        .delivery_rtt_us = 150000, .window = {.valid = 1}};
    call_audio_batch_init(&c, 0);
    CHECK(c.target == 4);
    CHECK(call_audio_batch_step(&c, 0, NULL, NULL) == 0 && c.target == 4);
    n.ready = 0; n.acked_packets = 0; n.window.valid = 0;
    step(&c, &n, 50000); CHECK(c.target == 4);
    n.ready = 1; n.acked_packets = 1;
    step(&c, &n, 100000); CHECK(c.target == 4 && !c.feedback_received);
    n.window.valid = 1;
    for (uint64_t t = 150000; t <= 10100000; t += 50000) step(&c, &n, t);
    CHECK(c.target == 4); // Постоянный RTT150мс не означает перегрузку; ждём10с устойчивости.
    step(&c, &n, 10150000); CHECK(c.target == 3 && c.probe_previous == 4);
    for (uint64_t t = 10200000; t <= 15200000; t += 50000) step(&c, &n, t);
    CHECK(c.target == 3 && !c.probe_previous); // Уменьшение принято после5с устойчивой пробы.
    for (uint64_t t = 15250000; t <= 25250000; t += 50000) step(&c, &n, t);
    CHECK(c.target == 2 && c.probe_previous == 3);
    // Доставка и EWMA растут вместе: прежний регулятор принимал это за постоянный RTT.
    for (uint64_t t = 25300000; t <= 25500000; t += 50000) {
        n.delivery_rtt_us += 20000; n.inflight_age_us = n.delivery_rtt_us; step(&c, &n, t);
    }
    CHECK(c.target == 3 && !c.probe_previous && c.probe_backoff_us == 60000000);
    uint64_t retry_at = c.probe_after_us;
    n.delivery_rtt_us = 150000; n.inflight_age_us = 0;
    for (uint64_t t = 25550000; t < retry_at; t += 50000) { step(&c, &n, t); CHECK(c.target == 3); }
    step(&c, &n, retry_at); CHECK(c.target == 2 && c.probe_previous == 3);
    n.receive_busy = 1; step(&c, &n, retry_at + 50000);
    CHECK(c.target == 3 && !c.probe_previous && c.probe_backoff_us == 120000000);
    n.receive_busy = 0;
    for (uint64_t t = retry_at + 100000; t <= retry_at + 8000000; t += 50000) {
        n.delivery_rtt_us += 15000; n.inflight_age_us = n.delivery_rtt_us; step(&c, &n, t);
    }
    CHECK(c.target == 10); // Медленно растущий EWMA не скрывает накопление очереди.
    n.peer_epoch = 3; n.inflight_age_us = 0; n.delivery_rtt_us = 20000;
    step(&c, &n, retry_at + 8050000); CHECK(c.target == 4 && c.probe_backoff_us == 30000000);
    n.queued_age_us = 150000; n.queued_packets = 2;
    uint64_t ack = n.acked_packets;
    for (uint64_t t = retry_at + 8100000; t <= retry_at + 10000000; t += 50000) {
        n.last_ack_us = t; CHECK(!call_audio_batch_step(&c, t, &n, NULL));
    }
    CHECK(c.target == 5 && n.acked_packets == ack); // После одного изменения старые ACK не разрешают следующее.
    CHECK(call_audio_batch_step(&c, c.last_us - 1, &n, NULL) == -1);
    CHECK(!call_audio_batch_step(&c, c.last_us + 1, NULL, NULL) && c.target == 4);
}

static void local_queue_and_path_test(void) {
    struct call_audio_batch_control c;
    struct ETCP_ROUTER_TX_STATS n = {.local_epoch = 1, .peer_epoch = 2, .ready = 1,
        .delivery_rtt_us = 20000, .window = {.valid = 1}};
    struct call_audio_batch_input input = {.path_node_id = 17, .path_epoch = 42, .frames = 3, .oldest_us = 300000};
    call_audio_batch_init(&c, 0);
    for (uint64_t t = 50000; t <= 1000000; t += 50000) {
        n.last_ack_us = t; ++n.acked_packets; CHECK(!call_audio_batch_step(&c, t, &n, &input));
        CHECK(c.target == 4); // Неполная пачка не доказывает backpressure даже при прерванном захвате.
    }
    input.frames = 4;
    for (uint64_t t = 1050000; t <= 1250000; t += 50000) {
        n.last_ack_us = t; ++n.acked_packets; CHECK(!call_audio_batch_step(&c, t, &n, &input));
    }
    CHECK(c.target == 5); // send_q Router пуст, но готовая локальная пачка застряла.
    input.path_node_id = 18; input.frames = 0; input.oldest_us = 0;
    n.last_ack_us = 1300000; ++n.acked_packets;
    CHECK(!call_audio_batch_step(&c, 1300000, &n, &input) && c.target == 4 && c.path_node_id == 18);
    input.path_epoch++;
    n.last_ack_us = 1350000; ++n.acked_packets;
    CHECK(!call_audio_batch_step(&c, 1350000, &n, &input) && c.target == 4 && c.path_epoch == 43);
    // На хорошем канале сохраняется возможность дойти до одного кадра.
    for (uint64_t t = 1400000; t <= 60000000; t += 50000) {
        n.last_ack_us = t; ++n.acked_packets; CHECK(!call_audio_batch_step(&c, t, &n, &input));
    }
    CHECK(c.target == 1 && !c.probe_previous);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    wire_test(); control_test(); local_queue_and_path_test();
    debug_set_level(DEBUG_LEVEL_INFO);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS audio batch: wire, stable RTT, failed probes/backoff, growing delay, local FIFO, ACK/route/epoch guards");
    return 0;
}
