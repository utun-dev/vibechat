#include "stream_address_fixture.h"
/* Виртуальное устройство сохраняет уже поставленные темпы: проверка догона и истории без реального времени. */
#include <stdlib.h>
#include <string.h>

#include "../src/stream/stream_playback_clock.h"
#include "../src/stream/stream_media_buffer.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

static void test_modes(void) {
    struct stream_playback_clock c; struct stream_playback_state s;
    stream_playback_clock_reset(&c, 90);
    CHECK(c.mode == STREAM_PLAYBACK_LOW_GLITCHES);
    CHECK(stream_playback_prefill_ms(c.mode) == 750 && stream_playback_ahead_us(c.mode) == 1000000);
    for (int64_t t = 0; t <= 31000000; t += 10000) {
        int64_t reserve = t == 1000000 ? 510000 : 900000;
        CHECK(!stream_playback_clock_update(&c, t, t, t + reserve, 20000, 0, 0, &s));
        CHECK(s.guard_us == 520000);
        if (t >= 1000000 && t <= 31000000) CHECK(s.minimum_us == 510000);
        if (t > 1500000 && t <= 31000000) CHECK(s.tempo_milli == 1000);
    }
    CHECK(!stream_playback_clock_update(&c, 31060000, 31060000, 31960000, 20000, 0, 0, &s));
    CHECK(s.minimum_us == 900000 && s.removable_us == 380000);
    int64_t correction = c.correction_us;
    CHECK(!stream_playback_clock_set_mode(&c, STREAM_PLAYBACK_LOW_LATENCY));
    CHECK(c.correction_us == correction && !stream_playback_clock_update(&c, 31070000, 31070000, 31970000, 20000, 0, 0, &s));
    CHECK(s.guard_us == 20000 && s.minimum_us == 900000);
    CHECK(!stream_playback_clock_set_mode(&c, STREAM_PLAYBACK_LOW_GLITCHES));
    CHECK(!stream_playback_clock_can_present(&c, 100000, 20000, 0));
    CHECK(stream_playback_clock_can_present(&c, 520000, 20000, 0));
    c.prefilling = 1; CHECK(stream_playback_clock_can_present(&c, 0, 20000, 1));
    CHECK(stream_playback_clock_set_mode(&c, 99) < 0);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] modes: default glitches, 30s retention/expiry, protected 500ms, live switch, prefill/EOF");
}

static void test_history_correction(void) {
    struct stream_playback_clock clock; struct stream_playback_state state;
    stream_playback_clock_reset(&clock, 1);
    stream_playback_clock_set_mode(&clock, STREAM_PLAYBACK_LOW_LATENCY);
    CHECK(!stream_playback_clock_update(&clock, 0, 0, 120000, 20000, 0, 0, &state));
    CHECK(!stream_playback_clock_update(&clock, 50000, 50000, 350000, 20000, 0, 0, &state));
    CHECK(state.minimum_us == 120000 && state.correction_us == 0);
    CHECK(!stream_playback_clock_update(&clock, 130000, 50000, 350000, 20000, 0, 1, &state));
    CHECK(state.minimum_us == 200000 && state.correction_us == 80000 && state.waiting);
    CHECK(!stream_playback_clock_update(&clock, 140000, 60000, 360000, 20000, 0, 0, &state));
    CHECK(!state.waiting && state.tempo_milli == 1000);
    CHECK(!stream_playback_clock_update(&clock, 150000, 90000, 390000, 20000, 0, 0, &state));
    CHECK(state.minimum_us == 180000 && state.correction_us == 60000);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] history: pause shifts minima +80ms, actual media progress repays 20ms");
}

static void test_catchup_device_queue(void) {
    for (unsigned pause_ms = 200; pause_ms <= 4000; pause_ms += 3800) {
        struct stream_playback_clock clock; struct stream_playback_state state;
        stream_playback_clock_reset(&clock, pause_ms);
        stream_playback_clock_set_mode(&clock, STREAM_PLAYBACK_LOW_LATENCY);
        /* 250ms prepared PCM + 100ms устройства: старый темп нельзя заменить задним числом. */
        int queued[350]; for (unsigned i = 0; i < 350; i++) queued[i] = 1000;
        int64_t position = 0, recovered_ms = -1;
        int maximum = 1000;
        for (int64_t tick = 0; tick <= 30000; tick++) {
            unsigned slot = (unsigned)(tick % 350);
            int starved = tick >= 1000 && tick < 1000 + pause_ms;
            if (tick && !starved) position += queued[slot];
            int64_t pending_gain = 0;
            for (unsigned i = 0; i < 350; i++) pending_gain += queued[i] - 1000;
            CHECK(!stream_playback_clock_update(&clock, tick * 1000, position, tick * 1000 + 60000,
                                                60000, pending_gain, starved, &state));
            CHECK(state.tempo_milli >= 1000 && state.tempo_milli <= STREAM_PLAYBACK_MAX_TEMPO);
            if (starved) CHECK(state.tempo_milli == 1000);
            if (state.tempo_milli > maximum) maximum = state.tempo_milli;
            CHECK(state.reserve_us >= 59000);
            if (tick > 1000 + pause_ms && state.reserve_us < 63000 && recovered_ms < 0) recovered_ms = tick;
            queued[slot] = state.tempo_milli;
        }
        CHECK(maximum > 1040 && state.history_ready && recovered_ms > 0 && recovered_ms < 25000);
        CHECK(state.reserve_us < 63000 && state.tempo_milli == 1000);
        if (pause_ms == 4000) CHECK(maximum == 1600);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] catchup: pause=%ums queued=350ms recovered_at=%lldms max_tempo=%d reserve=%lldus",
                   pause_ms, (long long)recovered_ms, maximum, (long long)state.reserve_us);
    }
}

static void test_invalid_and_sampling_gap(void) {
    struct stream_playback_clock clock; struct stream_playback_state state, before;
    stream_playback_clock_reset(&clock, 3); memset(&state, 0xa5, sizeof(state)); before = state;
    CHECK(stream_playback_clock_update(&clock, -1, 0, 0, 0, 0, 0, &state) < 0 && !memcmp(&state, &before, sizeof(state)));
    CHECK(!stream_playback_clock_update(&clock, 0, 0, 200000, 20000, 0, 0, &state));
    CHECK(!stream_playback_clock_update(&clock, 2000000, 2000000, 2200000, 20000, 0, 0, &state));
    CHECK(!state.stable_us && state.tempo_milli == 1000 && !state.history_ready);
    before = state;
    CHECK(stream_playback_clock_update(&clock, 2000001, 1000000, 2200000, 20000, 0, 0, &state) < 0);
    CHECK(!memcmp(&state, &before, sizeof(state)));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] clock: invalid/backward positions preserve result, missed samples restart observation");
}

static struct stream_descriptor descriptor = {.group_id = 1, .group_key = {1, [31] = 1},  .source_epoch = 3,
    .n = 8, .r = 2, .block_size = STREAM_RS_DEFAULT_SIZE};
static uint8_t video_bytes[128], opus_bytes[10];

/* Исходный медиаблок хранится только в RS; индекс получает проверенное восстановленное содержимое. */
static void append_media(struct stream_encoder *encoder, struct stream_buffer *buffer, struct stream_media_store *store,
                         uint64_t seq, uint64_t pts, int key, int parity_first) {
    struct stream_media_block b = {.format = {64, 64, 0, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1},
        .video_count = 1, .audio_count = 2};
    b.video[0] = (struct stream_media_video){seq + 1, pts, sizeof(video_bytes), 0, sizeof(video_bytes), key, video_bytes};
    b.audio[0] = (struct stream_media_audio){pts, sizeof(opus_bytes), 960, opus_bytes};
    b.audio[1] = (struct stream_media_audio){pts + 20000, sizeof(opus_bytes), 960, opus_bytes};
    uint8_t wire[1024], restored[1024]; size_t length = 0, part_size = 0, decoded = 0;
    CHECK(stream_media_encode(&b, wire, sizeof(wire), &length) == STREAM_OK);
    const uint8_t *parts[RS_MAX_BLOCKS];
    CHECK(stream_encoder_encode(encoder, seq, (uint16_t)(pts / 1000), 0, wire, length, parts, &part_size) == STREAM_OK);
    for (unsigned i = parity_first ? 2 : 0; i < 8; i++) CHECK(stream_buffer_put(buffer, parts[i], part_size) == STREAM_OK);
    CHECK(stream_buffer_decode(buffer, seq, restored, sizeof(restored), &decoded) == STREAM_OK);
    CHECK(decoded == length && !memcmp(restored, wire, length));
    CHECK(stream_media_store_feed(store, seq, restored, decoded) == STREAM_OK);
    if (parity_first) {
        uint8_t bad[1300]; memcpy(bad, parts[0], part_size); bad[STREAM_BLOCK_RS_OFF + RS_HEADER_SIZE] ^= 1;
        CHECK(stream_buffer_put(buffer, bad, part_size) == STREAM_OK);
        CHECK(stream_buffer_decode(buffer, seq, restored, sizeof(restored), &decoded) == STREAM_OK);
        CHECK(decoded == length && !memcmp(restored, wire, length));
    }
}

/* Два курсора делят кодированные данные; быстрый reader и late part не меняют результат медленного. */
static void test_shared_media_ring(void) {
    uint8_t key[32] = {1}; struct stream_encoder *encoder = NULL; struct stream_buffer *buffer = NULL;
    struct stream_media_store *store = NULL; struct stream_media_cursor *fast = NULL, *slow = NULL;
    descriptor.source = stream_test_public(key);
    CHECK(stream_encoder_create(&encoder, &descriptor, key) == STREAM_OK);
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 32 * 1300) == STREAM_OK);
    CHECK(stream_media_store_create(&store, buffer, &descriptor) == STREAM_OK);
    CHECK(stream_media_cursor_create(&fast, store, 3) == STREAM_OK && stream_media_cursor_create(&slow, store, 3) == STREAM_OK);
    append_media(encoder, buffer, store, 0, 0, STREAM_MEDIA_KEY, 1);
    struct stream_media_status status;
    CHECK(stream_media_cursor_status(fast, &status) == STREAM_OK && status.initialized && status.ready_us == 0);
    CHECK(stream_media_cursor_status(slow, &status) == STREAM_OK && status.initialized && status.start_us == 0);
    append_media(encoder, buffer, store, 1, 40000, 0, 0);
    append_media(encoder, buffer, store, 2, 80000, 0, 0);
    struct stream_media_packet packet;
    CHECK(stream_media_cursor_read(fast, -1, 1, 0, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_VIDEO);
    CHECK(packet.video.pts_us == 0 && !memcmp(packet.video.data, video_bytes, sizeof(video_bytes)));
    unsigned videos = 1, audio = 0;
    for (unsigned step = 0; step < 20; step++) {
        int result = stream_media_cursor_read(fast, 80000, 1, 0, &packet);
        if (result == STREAM_WAIT) break;
        CHECK(result == STREAM_OK);
        if (packet.kind == STREAM_MEDIA_READ_VIDEO) videos++;
        else { CHECK(packet.kind == STREAM_MEDIA_READ_AUDIO && packet.audio.pts_us <= 80000); audio++; }
    }
    CHECK(videos == 3 && audio == 5);
    CHECK(stream_media_cursor_read(slow, -1, 1, 0, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_VIDEO);
    CHECK(packet.video.pts_us == 0 && !memcmp(packet.video.data, video_bytes, sizeof(video_bytes)));
    append_media(encoder, buffer, store, 3, 120000, STREAM_MEDIA_KEY, 0);
    CHECK(stream_media_cursor_status(slow, &status) == STREAM_OK && status.start_us == 0 && status.ready_us == 120000);
    stream_media_store_gap(store, 4, 4);
    append_media(encoder, buffer, store, 5, 200000, STREAM_MEDIA_KEY, 0);
    CHECK(stream_media_cursor_read(slow, 0, 1, 0, &packet) == STREAM_EXPIRED);
    int gap = 0;
    for (unsigned step = 0; step < 20; step++) {
        int result = stream_media_cursor_read(fast, 180000, 1, 0, &packet);
        if (result == STREAM_GAP) { gap = 1; break; }
        CHECK(result == STREAM_OK || result == STREAM_WAIT);
    }
    CHECK(gap);
    stream_media_cursor_reset(fast);
    CHECK(stream_media_cursor_read(fast, -1, 1, 0, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_VIDEO);
    CHECK(packet.video.pts_us == 200000);
    stream_media_store_finish(store);
    int ended = 0;
    for (unsigned step = 0; step < 20; step++) {
        int result = stream_media_cursor_read(fast, 1000000, 1, 0, &packet);
        CHECK(result == STREAM_OK || result == STREAM_WAIT);
        if (result == STREAM_OK && packet.kind == STREAM_MEDIA_READ_END) { ended = 1; break; }
    }
    CHECK(ended);
    stream_media_cursor_destroy(fast); stream_media_cursor_destroy(slow); stream_media_store_destroy(store);
    stream_buffer_destroy(buffer); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] media ring: independent readers, authenticated frozen parts, lookahead, expiry, GAP and EOF");
}

/* Конвейер decoder получает восемь кадров при неподвижных часах; запрет видео сохраняет его cursor. */
static void test_decoder_demand(void) {
    uint8_t key[32] = {4}; struct stream_encoder *encoder = NULL; struct stream_buffer *buffer = NULL;
    struct stream_media_store *store = NULL; struct stream_media_cursor *cursor = NULL;
    descriptor.source = stream_test_public(key);
    CHECK(stream_encoder_create(&encoder, &descriptor, key) == STREAM_OK);
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 256 * 1300) == STREAM_OK);
    CHECK(stream_media_store_create(&store, buffer, &descriptor) == STREAM_OK);
    CHECK(stream_media_cursor_create(&cursor, store, 3) == STREAM_OK);
    for (unsigned i = 0; i < 8; i++) append_media(encoder, buffer, store, i, i * 66666u, i ? 0 : STREAM_MEDIA_KEY, 0);
    struct stream_media_packet packet;
    CHECK(stream_media_cursor_read(cursor, 0, 0, -1, &packet) == STREAM_WAIT);
    for (unsigned i = 0; i < 8; i++) {
        CHECK(stream_media_cursor_read(cursor, 0, 1, -1, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_VIDEO);
        CHECK(packet.video.pts_us == i * 66666u);
        if (i == 2) {
            CHECK(stream_media_cursor_read(cursor, 0, 0, 0, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_AUDIO);
            CHECK(packet.audio.pts_us == 0);
        }
    }
    CHECK(stream_media_cursor_read(cursor, 0, 2, -1, &packet) == STREAM_ERR_ARGUMENT);
    stream_media_cursor_destroy(cursor); stream_media_store_destroy(store); stream_buffer_destroy(buffer); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] decoder demand: eight frames beyond 80ms at frozen PTS, independent audio and disabled video cursor");
}

/* Полный кадр уже в общем кольце: MORE продолжает ограниченную сборку без новых сетевых событий. */
static void test_large_frame_budget(void) {
    uint8_t key[32] = {2}; struct stream_encoder *encoder = NULL; struct stream_buffer *buffer = NULL;
    struct stream_media_store *store = NULL; struct stream_media_cursor *cursor = NULL;
    descriptor.source = stream_test_public(key);
    CHECK(stream_encoder_create(&encoder, &descriptor, key) == STREAM_OK);
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 1024 * 1300) == STREAM_OK);
    CHECK(stream_media_store_create(&store, buffer, &descriptor) == STREAM_OK);
    CHECK(stream_media_cursor_create(&cursor, store, STREAM_MEDIA_TRACK_VIDEO) == STREAM_OK);
    size_t frame_size = 200000; uint8_t *frame = u_malloc((uint32_t)frame_size); CHECK(frame);
    for (size_t i = 0; i < frame_size; i++) frame[i] = (uint8_t)(i * 19 + 7);
    unsigned seq = 0;
    for (size_t offset = 0; offset < frame_size; seq++) {
        size_t take = frame_size - offset; if (take > 6000) take = 6000;
        struct stream_media_block media = {.format = {64, 64, 0, STREAM_MEDIA_H264, 0, 1}, .video_count = 1};
        media.video[0] = (struct stream_media_video){1, 1000000, (uint32_t)frame_size, (uint32_t)offset,
            (uint32_t)take, STREAM_MEDIA_KEY, frame + offset};
        uint8_t data[7345], decoded[7345]; size_t length = 0, decoded_length = 0, part_size = 0;
        const uint8_t *parts[RS_MAX_BLOCKS];
        CHECK(stream_media_encode(&media, data, sizeof(data), &length) == STREAM_OK);
        CHECK(stream_encoder_encode(encoder, seq, 1, 0, data, length, parts, &part_size) == STREAM_OK);
        for (unsigned sub = 0; sub < 8; sub++) CHECK(stream_buffer_put(buffer, parts[sub], part_size) == STREAM_OK);
        CHECK(stream_buffer_decode(buffer, seq, decoded, sizeof(decoded), &decoded_length) == STREAM_OK);
        CHECK(stream_media_store_feed(store, seq, decoded, decoded_length) == STREAM_OK);
        offset += take;
    }
    struct stream_media_packet packet; unsigned more = 0; int result;
    do { result = stream_media_cursor_read(cursor, -1, 1, -1, &packet); if (result == STREAM_MORE) more++; }
    while (result == STREAM_MORE && more < 10);
    CHECK(result == STREAM_OK && more >= 2 && packet.kind == STREAM_MEDIA_READ_VIDEO);
    CHECK(packet.video.length == frame_size && !memcmp(packet.video.data, frame, frame_size));
    stream_media_store_finish(store);
    CHECK(stream_media_cursor_read(cursor, 1000000, 1, -1, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_END);
    stream_media_cursor_destroy(cursor); stream_media_store_destroy(store); stream_buffer_destroy(buffer);
    stream_encoder_destroy(encoder); u_free(frame);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] large frame: bounded MORE work, complete existing-ring assembly and video-only EOF");
}

/* Повреждённая последовательность кадров не оставляет старую границу готовности и decoder references. */
static void test_media_discontinuity(void) {
    uint8_t key[32] = {3}; struct stream_encoder *encoder = NULL; struct stream_buffer *buffer = NULL;
    struct stream_media_store *store = NULL; struct stream_media_cursor *cursor = NULL, *fresh = NULL;
    descriptor.source = stream_test_public(key);
    CHECK(stream_encoder_create(&encoder, &descriptor, key) == STREAM_OK);
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 64 * 1300) == STREAM_OK);
    CHECK(stream_media_store_create(&store, buffer, &descriptor) == STREAM_OK);
    CHECK(stream_media_cursor_create(&cursor, store, 3) == STREAM_OK);
    CHECK(stream_media_cursor_create(&fresh, store, 3) == STREAM_OK);
    append_media(encoder, buffer, store, 0, 0, STREAM_MEDIA_KEY, 0);
    struct stream_media_packet packet; struct stream_media_status status;
    CHECK(stream_media_cursor_read(cursor, -1, 1, 0, &packet) == STREAM_OK);
    struct stream_media_block media = {.format = {64, 64, 0, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1}, .video_count = 1};
    media.video[0] = (struct stream_media_video){2, 40000, 256, 0, 128, 0, video_bytes};
    uint8_t wire[1024]; size_t length = 0;
    CHECK(stream_media_encode(&media, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_media_store_feed(store, 1, wire, length) == STREAM_OK);
    append_media(encoder, buffer, store, 2, 80000, 0, 0);
    CHECK(stream_media_cursor_status(cursor, &status) == STREAM_OK && status.video_us == 0);
    CHECK(stream_media_cursor_status(fresh, &status) == STREAM_OK && !status.initialized);
    stream_media_cursor_reset(cursor);
    append_media(encoder, buffer, store, 3, 120000, STREAM_MEDIA_KEY, 0);
    CHECK(stream_media_cursor_status(cursor, &status) == STREAM_OK && status.generation == 2 && status.start_us == 120000);
    append_media(encoder, buffer, store, 4, 400000, 0, 0);
    stream_media_cursor_reset(cursor);
    CHECK(stream_media_cursor_status(cursor, &status) == STREAM_OK && !status.initialized);
    append_media(encoder, buffer, store, 5, 440000, STREAM_MEDIA_KEY, 0);
    CHECK(stream_media_cursor_status(cursor, &status) == STREAM_OK && status.generation == 3 && status.start_us == 440000);
    media.format.rotation = 90; media.video[0].frame_id = 7; media.video[0].pts_us = 480000;
    CHECK(stream_media_encode(&media, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_media_store_feed(store, 6, wire, length) == STREAM_ERR_CONFLICT);
    stream_media_cursor_destroy(cursor); stream_media_cursor_destroy(fresh); stream_media_store_destroy(store); store = NULL;
    CHECK(stream_media_store_create(&store, buffer, &descriptor) == STREAM_OK);
    media.video_count = 0; media.audio_count = 1;
    media.audio[0] = (struct stream_media_audio){INT64_MAX, sizeof(opus_bytes), 960, opus_bytes};
    CHECK(stream_media_encode(&media, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_media_store_feed(store, 0, wire, length) == STREAM_ERR_FORMAT);
    stream_media_store_destroy(store); stream_buffer_destroy(buffer); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] media discontinuity: incomplete video and audio gap require KEY; format and PTS bounds enforced");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    size_t allocations = u_get_allocated_count();
    for (unsigned i = 0; i < sizeof(video_bytes); i++) video_bytes[i] = (uint8_t)(i * 71 + 3);
    test_modes(); test_history_correction(); test_catchup_device_queue(); test_invalid_and_sampling_gap(); test_shared_media_ring();
    test_decoder_demand(); test_large_frame_budget(); test_media_discontinuity();
    CHECK(u_get_allocated_count() == allocations);
    return 0;
}
