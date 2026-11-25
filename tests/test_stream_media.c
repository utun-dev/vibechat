#include "stream_address_fixture.h"
/* Границы FEC-пачек, все пары потерь, ненулевой offset начала, malformed wire и восстановление после GAP. */
#include <stdlib.h>
#include <string.h>

#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../src/stream/stream_source.h"
#include "../src/stream/stream_media.h"
#include "../src/stream/stream_media_buffer.h"

#define CHECK(x) do { if (!(x)) { debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_ERROR); DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream media test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

static unsigned batches;
static uint8_t expected_video[9000];
static void ready(struct stream_source *source, uint64_t seq, void *arg) {
    (void)source; (void)arg; CHECK(seq == batches++);
}

/* Канонический формат и сохранение выходного view при ошибке всех классов полей. */
static void test_media(void) {
    struct stream_media_block b = {.format = {1280, 720, 90, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS, 1}, .video_count = 1, .audio_count = 1};
    b.video[0] = (struct stream_media_video){1, 1000000, sizeof(expected_video), 0, 6000, STREAM_MEDIA_KEY, expected_video};
    b.audio[0] = (struct stream_media_audio){1000000, 3, 960, (const uint8_t *)"ops"};
    uint8_t wire[7080], saved[7080]; size_t length = 0;
    CHECK(stream_media_encode(&b, wire, sizeof(wire), &length) == STREAM_OK);
    memcpy(saved, wire, length);
    struct stream_media_block parsed;
    CHECK(stream_media_parse(wire, length, &parsed) == STREAM_OK && parsed.first_frame_offset == 0);
    CHECK(parsed.audio_count == 1 && parsed.audio[0].length == 3 && !memcmp(parsed.audio[0].data, "ops", 3));
    const unsigned mutations[] = {0, 4, 5, 6, 7, 8, 12, 14, 16, 20, 24, 48, 52, 56, 60, 61, 72, 76, 78};
    for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        memcpy(wire, saved, length); wire[mutations[i]] = 255;
        struct stream_media_block before; memset(&before, 0xa5, sizeof(before)); parsed = before;
        CHECK(stream_media_parse(wire, length, &parsed) < 0 && !memcmp(&parsed, &before, sizeof(before)));
    }
    for (size_t i = 0; i < length; i++) CHECK(stream_media_parse(saved, i, &parsed) < 0);
    b.video[0].offset = 6000; b.video[0].length = 3000; b.video[0].data = expected_video + 6000;
    b.audio_count = 0;
    CHECK(stream_media_encode(&b, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_media_parse(wire, length, &parsed) == STREAM_OK && parsed.first_frame_offset == UINT32_MAX);
    b.video_count = 2;
    b.video[1] = (struct stream_media_video){2, 1040000, sizeof(expected_video), 0, 3000, STREAM_MEDIA_KEY, expected_video};
    CHECK(stream_media_encode(&b, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_media_parse(wire, length, &parsed) == STREAM_OK && parsed.first_frame_offset == 3000);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS media: nonzero frame start, malformed fields and atomic parser output");
}

/* Две короткие записи остаются двумя пачками; каждый медиазаголовок переживает любую пару потерь 6+2. */
static void test_batches(void) {
    struct stream_descriptor d = {.group_id = 1, .group_key = {1, [31] = 1},  .source_epoch = 3, .n = 8, .r = 2,
        .block_size = STREAM_RS_DEFAULT_SIZE};
    uint8_t key[32] = {1}; struct stream_encoder *encoder = NULL;
    d.source = stream_test_public(key);
    CHECK(stream_encoder_create(&encoder, &d, key) == STREAM_OK);
    struct stream_media_block b = {.format = {1280, 720, 90, STREAM_MEDIA_H264, 0, 1}, .video_count = 1};
    b.video[0] = (struct stream_media_video){1, 1000000, 100, 0, 100, STREAM_MEDIA_KEY, expected_video};
    uint8_t input[7080], output[7080]; size_t length = 0;
    CHECK(stream_media_encode(&b, input, sizeof(input), &length) == STREAM_OK);
    const uint8_t *blocks[RS_MAX_BLOCKS];
    size_t part_size = 0;
    CHECK(stream_encoder_encode(encoder, 0, 1, 0, input, length, blocks, &part_size) == STREAM_OK && part_size < 1300);
    for (unsigned a = 0; a < 8; a++) for (unsigned c = a + 1; c < 8; c++) {
        struct stream_buffer *buffer = NULL; size_t decoded = 0;
        CHECK(stream_buffer_create(&buffer, &d, stream_encoder_public_key(encoder), 8 * 1300) == STREAM_OK);
        for (unsigned i = 0; i < 8; i++) if (i != a && i != c) CHECK(stream_buffer_put(buffer, blocks[i], part_size) == STREAM_OK);
        CHECK(stream_buffer_decode(buffer, 0, output, sizeof(output), &decoded) == STREAM_OK);
        CHECK(decoded == length && !memcmp(input, output, length));
        struct stream_media_block parsed; CHECK(stream_media_parse(output, decoded, &parsed) == STREAM_OK);
        stream_buffer_destroy(buffer);
    }
    struct UASYNC *ua = uasync_create(); struct stream_source *source = NULL;
    CHECK(ua && stream_source_create(&source, ua, &d, key, ready, NULL) == STREAM_OK);
    CHECK(stream_source_write_block(source, input, length) == STREAM_OK && batches == 1);
    CHECK(stream_source_write_block(source, input, length) == STREAM_OK && batches == 2);
    size_t accepted = 10;
    CHECK(stream_source_write(source, input, length, &accepted) == STREAM_ERR_ARGUMENT && accepted == 0);
    CHECK(stream_source_finish(source) == STREAM_OK);
    CHECK(stream_source_write_block(source, input, length) == STREAM_ERR_STATE);
    stream_source_destroy(source);
    CHECK(stream_source_create(&source, ua, &d, key, ready, NULL) == STREAM_ERR_ARGUMENT);
    source = NULL;
    CHECK(stream_source_create(&source, ua, &d, key, NULL, NULL) == STREAM_OK);
    CHECK(stream_source_write(source, input, length, &accepted) == STREAM_OK && accepted == length);
    CHECK(stream_source_write_block(source, input, length) == STREAM_ERR_ARGUMENT);
    stream_source_destroy(source); uasync_destroy(ua, 0); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS batches: atomic boundaries, all FEC losses and mode isolation");
}

/* Стерео обозначается в wire явно; 960 — число кадров на канал. */
static void test_stereo(void) {
    struct stream_media_block b = {.format = {1280, 720, 0, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1}, .audio_count = 1};
    b.audio[0] = (struct stream_media_audio){1000000, 3, 960, (const uint8_t *)"ops"};
    uint8_t wire[7080]; size_t length = 0;
    CHECK(stream_media_encode(&b, wire, sizeof(wire), &length) == STREAM_OK);
    struct stream_media_block parsed;
    CHECK(stream_media_parse(wire, length, &parsed) == STREAM_OK && parsed.format.audio_codec == STREAM_MEDIA_OPUS_STEREO);
    CHECK(parsed.audio[0].samples == 960 && parsed.audio[0].length == 3 && !memcmp(parsed.audio[0].data, "ops", 3));
    b.audio[0].samples = 1920;
    CHECK(stream_media_encode(&b, wire, sizeof(wire), &length) == STREAM_ERR_FORMAT);
    b.audio[0].samples = 960; b.format.audio_codec = 3;
    CHECK(stream_media_encode(&b, wire, sizeof(wire), &length) == STREAM_ERR_FORMAT);
}

static void test_codecs(void) {
    uint8_t wire[256], key[] = {0, 0, 0, 2, 11, 12, 13}; size_t length = 0;
    struct stream_media_block b = {.format = {64, 64, 0, STREAM_MEDIA_H264, 0, 1}, .video_count = 1};
    b.video[0] = (struct stream_media_video){1, 0, sizeof(key), 0, sizeof(key), STREAM_MEDIA_KEY, key};
    struct stream_media_block parsed;
    for (unsigned codec = STREAM_MEDIA_H264; codec <= STREAM_MEDIA_H266; codec++) {
        b.format.video_codec = codec;
        CHECK(stream_media_encode(&b, wire, sizeof(wire), &length) == STREAM_OK && !memcmp(wire, "uAV2", 4));
        CHECK(stream_media_parse(wire, length, &parsed) == STREAM_OK && parsed.format.video_codec == codec);
    }
    wire[3] = '1'; CHECK(stream_media_parse(wire, length, &parsed) == STREAM_ERR_FORMAT);
    const uint8_t *config, *frame; size_t config_size, frame_size;
    CHECK(stream_media_key_parse(key, sizeof(key), &config, &config_size, &frame, &frame_size) == STREAM_OK);
    CHECK(config_size == 2 && frame_size == 1 && config[0] == 11 && frame[0] == 13);
    for (size_t n = 0; n < sizeof(key); n++) CHECK(stream_media_key_parse(key, n, &config, &config_size, &frame, &frame_size) < 0);
    key[0] = 1; CHECK(stream_media_key_parse(key, sizeof(key), &config, &config_size, &frame, &frame_size) < 0);
    memset(key, 0, 4); CHECK(stream_media_key_parse(key, sizeof(key), &config, &config_size, &frame, &frame_size) == STREAM_OK);
}

struct media_fixture {
    struct stream_encoder *encoder;
    struct stream_buffer *buffer;
    struct stream_media_store *store;
    struct stream_media_cursor *cursor;
    uint64_t seq;
};

static void media_fixture_open(struct media_fixture *f) {
    struct stream_descriptor d = {.group_id = 1, .group_key = {1, [31] = 1},  .source_epoch = 3,
        .n = 8, .r = 2, .block_size = STREAM_RS_DEFAULT_SIZE};
    uint8_t key[32] = {1}; memset(f, 0, sizeof(*f));
    d.source = stream_test_public(key);
    CHECK(stream_encoder_create(&f->encoder, &d, key) == STREAM_OK);
    CHECK(stream_buffer_create(&f->buffer, &d, stream_encoder_public_key(f->encoder), 128 * 1300) == STREAM_OK);
    CHECK(stream_media_store_create(&f->store, f->buffer, &d) == STREAM_OK);
    CHECK(stream_media_cursor_create(&f->cursor, f->store, STREAM_MEDIA_TRACK_VIDEO | STREAM_MEDIA_TRACK_AUDIO) == STREAM_OK);
}

static void media_fixture_close(struct media_fixture *f) {
    stream_media_cursor_destroy(f->cursor); stream_media_store_destroy(f->store);
    stream_buffer_destroy(f->buffer); stream_encoder_destroy(f->encoder);
}

/* Проверяем настоящий путь: сериализация, RS-кольцо, проверка подписи, индекс и независимый cursor. */
static int media_fixture_feed(struct media_fixture *f, const struct stream_media_block *b) {
    uint8_t wire[7080], decoded[7080]; size_t length = 0, part_size = 0, decoded_size = 0;
    const uint8_t *parts[RS_MAX_BLOCKS];
    CHECK(stream_media_encode(b, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_encoder_encode(f->encoder, f->seq, 1, 0, wire, length, parts, &part_size) == STREAM_OK);
    for (unsigned i = 0; i < 6; i++) CHECK(stream_buffer_put(f->buffer, parts[i], part_size) == STREAM_OK);
    CHECK(stream_buffer_decode(f->buffer, f->seq, decoded, sizeof(decoded), &decoded_size) == STREAM_OK);
    CHECK(length == decoded_size && !memcmp(wire, decoded, length));
    return stream_media_store_feed(f->store, f->seq++, decoded, decoded_size);
}

/* Формат внутри revision и аудио внутри эпохи неизменны; регрессия live revision запрещена. */
static void test_format_conflicts(void) {
    for (unsigned conflict = 0; conflict < 4; conflict++) {
        struct media_fixture f; media_fixture_open(&f);
        struct stream_media_block b = {.format = {1280, 720, 0, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 2},
            .video_count = 1};
        b.video[0] = (struct stream_media_video){1, 1000000, 12, 0, 12, STREAM_MEDIA_KEY, expected_video};
        CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
        if (conflict == 0) b.format.revision = 1;
        else if (conflict == 1) b.format.width++;
        else { b.format.revision++; b.format.audio_codec = conflict == 2 ? STREAM_MEDIA_OPUS : 0; }
        b.video[0].frame_id++; b.video[0].pts_us += 40000;
        CHECK(media_fixture_feed(&f, &b) == STREAM_ERR_CONFLICT);
        media_fixture_close(&f);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS media store: revision regression, conflicting video and changed/removed audio rejected");
}

/* Новая revision начинается с целого KEY; аудио и отстающий reader сохраняют собственные позиции. */
static void test_revision_transition(void) {
    struct media_fixture f; media_fixture_open(&f);
    struct stream_media_block b = {.format = {1280, 720, 90, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1},
        .video_count = 1, .audio_count = 1};
    b.video[0] = (struct stream_media_video){1, 1000000, 12, 0, 12, STREAM_MEDIA_KEY, expected_video};
    b.audio[0] = (struct stream_media_audio){1000000, 3, 960, (const uint8_t *)"ops"};
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    struct stream_media_packet packet; struct stream_media_status status; struct stream_media_cursor *slow = NULL;
    CHECK(stream_media_cursor_create(&slow, f.store, STREAM_MEDIA_TRACK_VIDEO) == STREAM_OK);
    CHECK(stream_media_cursor_status(slow, &status) == STREAM_OK && status.initialized);
    CHECK(stream_media_cursor_read(f.cursor, -1, 1, -1, &packet) == STREAM_OK && packet.format.revision == 1);
    b.audio_count = 0;
    b.video[0] = (struct stream_media_video){2, 1040000, 12, 0, 6, 0, expected_video};
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_cursor_read(f.cursor, 1000000, 1, -1, &packet) == STREAM_WAIT);
    b.format = (struct stream_media_format){1920, 1080, 270, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 2};
    b.video[0] = (struct stream_media_video){3, 1080000, 12, 0, 12, 0, expected_video};
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_cursor_status(f.cursor, &status) == STREAM_OK && status.video_us == 1000000);
    CHECK(stream_media_cursor_read(f.cursor, 1000000, 1, -1, &packet) == STREAM_WAIT);
    CHECK(stream_media_cursor_read(f.cursor, 1000000, 0, 0, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_AUDIO);
    CHECK(packet.audio.pts_us == 1000000 && packet.format.audio_codec == STREAM_MEDIA_OPUS_STEREO);
    b.video_count = 0; b.audio_count = 1; b.audio[0].pts_us = 1020000;
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_cursor_read(f.cursor, 1020000, 0, 0, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_AUDIO);
    CHECK(packet.audio.pts_us == 1020000);
    b.video_count = 1; b.audio_count = 0;
    b.video[0] = (struct stream_media_video){4, 1120000, 12, 0, 6, STREAM_MEDIA_KEY, expected_video};
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_cursor_read(f.cursor, 1000000, 1, -1, &packet) == STREAM_WAIT);
    b.video[0].offset = 6; b.video[0].data = expected_video + 6;
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_cursor_read(f.cursor, 1000000, 1, -1, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_VIDEO);
    CHECK(packet.format.revision == 2 && packet.video.frame_id == 4 && packet.video.flags == STREAM_MEDIA_KEY);
    CHECK(packet.video.length == 12 && !memcmp(packet.video.data, expected_video, 12));
    CHECK(stream_media_cursor_read(slow, -1, 1, -1, &packet) == STREAM_OK && packet.format.revision == 1);
    CHECK(stream_media_cursor_read(slow, 1000000, 1, -1, &packet) == STREAM_OK && packet.format.revision == 2);
    CHECK(packet.video.frame_id == 4);
    /* Полный KEY новой конфигурации выдаётся сразу, без таймера и искусственной задержки. */
    b.format.revision = 3; b.video[0] = (struct stream_media_video){5, 1160000, 12, 0, 12, STREAM_MEDIA_KEY, expected_video};
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_cursor_read(f.cursor, 1120000, 1, -1, &packet) == STREAM_OK && packet.format.revision == 3);
    CHECK(packet.video.frame_id == 5 && packet.video.flags == STREAM_MEDIA_KEY);
    stream_media_cursor_destroy(slow);
    media_fixture_close(&f);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS cursor revisions: missing/fragmented KEY, immediate complete KEY, independent audio and history");
}

/* Пропущенный блок, нарушенные фрагменты и вытеснение запрещают P-кадры до нового целого KEY. */
static void test_cursor_key_recovery(void) {
    for (unsigned failure = 0; failure < 3; failure++) {
        struct media_fixture f; media_fixture_open(&f);
        struct stream_media_block b = {.format = {1280, 720, 0, STREAM_MEDIA_H264, 0, 1}, .video_count = 1};
        b.video[0] = (struct stream_media_video){1, 1000000, 12, 0, 12, STREAM_MEDIA_KEY, expected_video};
        CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
        struct stream_media_packet packet;
        CHECK(stream_media_cursor_read(f.cursor, -1, 1, -1, &packet) == STREAM_OK);
        b.video[0] = (struct stream_media_video){2, 1040000, 12, 0, 6, 0, expected_video};
        CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
        CHECK(stream_media_cursor_read(f.cursor, 1000000, 1, -1, &packet) == STREAM_WAIT);
        if (failure == 0) { stream_media_store_gap(f.store, f.seq, f.seq); f.seq++; }
        else if (failure == 1) {
            b.video[0].offset = 5; b.video[0].data = expected_video + 5;
            CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
        } else {
            for (unsigned i = 0; i < 48; i++) {
                b.video[0] = (struct stream_media_video){3 + i, 1080000 + i * 40000, 12, 0, 12, 0, expected_video};
                CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
            }
        }
        int result = stream_media_cursor_read(f.cursor, 1000000, 1, -1, &packet);
        CHECK(result == (failure == 2 ? STREAM_EXPIRED : STREAM_GAP));
        stream_media_cursor_reset(f.cursor);
        b.video[0] = (struct stream_media_video){100, 4000000, 12, 0, 12, 0, expected_video};
        CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
        CHECK(stream_media_cursor_read(f.cursor, -1, 1, -1, &packet) == STREAM_WAIT);
        b.video[0] = (struct stream_media_video){101, 4040000, 12, 0, 6, STREAM_MEDIA_KEY, expected_video};
        CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
        CHECK(stream_media_cursor_read(f.cursor, -1, 1, -1, &packet) == STREAM_WAIT);
        b.video[0].offset = 6; b.video[0].data = expected_video + 6;
        CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
        CHECK(stream_media_cursor_read(f.cursor, -1, 1, -1, &packet) == STREAM_OK && packet.kind == STREAM_MEDIA_READ_VIDEO);
        CHECK(packet.video.frame_id == 101 && packet.video.flags == STREAM_MEDIA_KEY && packet.video.length == 12);
        CHECK(!memcmp(packet.video.data, expected_video, 12));
        media_fixture_close(&f);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS cursor recovery: GAP, fragment discontinuity and expiry resume only from a complete KEY");
}

/* Объявлять можно только целый сохранённый KEY; незавершённый новый кадр не заменяет предыдущую точку старта. */
static void test_recovery_positions(void) {
    struct media_fixture f; media_fixture_open(&f);
    struct stream_media_recovery position;
    CHECK(stream_media_store_recovery(f.store, &position) == STREAM_OK && !position.key_valid && position.last_seq == UINT64_MAX);
    CHECK(position.video_us == -1 && position.key_us == -1);
    struct stream_media_block b = {.format = {64, 64, 0, STREAM_MEDIA_H264, 0, 1}, .video_count = 1};
    b.video[0] = (struct stream_media_video){1, 0, 12, 0, 6, STREAM_MEDIA_KEY, expected_video};
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_store_recovery(f.store, &position) == STREAM_OK && !position.key_valid && position.last_seq == 0);
    CHECK(position.video_us == -1 && position.key_us == -1);
    b.video[0].offset = 6; b.video[0].data = expected_video + 6;
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_store_recovery(f.store, &position) == STREAM_OK && position.key_valid);
    CHECK(position.key_seq == 0 && position.key_last_seq == 1 && position.last_seq == 1);
    CHECK(position.video_us == 0 && position.key_us == 0);
    b.video[0] = (struct stream_media_video){2, 40000, 12, 0, 6, STREAM_MEDIA_KEY, expected_video};
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_store_recovery(f.store, &position) == STREAM_OK && position.key_valid);
    CHECK(position.key_seq == 0 && position.key_last_seq == 1 && position.last_seq == 2);
    CHECK(position.video_us == 0 && position.key_us == 0);
    b.video[0].offset = 6; b.video[0].data = expected_video + 6;
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_store_recovery(f.store, &position) == STREAM_OK && position.key_seq == 2 && position.key_last_seq == 3);
    CHECK(position.video_us == 40000 && position.key_us == 40000);
    for (unsigned i = 0; i < 48; i++) {
        b.video[0] = (struct stream_media_video){3 + i, 80000 + i * 40000, 12, 0, 12, 0, expected_video};
        CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    }
    CHECK(stream_media_store_recovery(f.store, &position) == STREAM_OK && !position.key_valid && position.last_seq == f.seq - 1);
    CHECK(position.video_us == 1960000 && position.key_us == -1);
    b.video[0] = (struct stream_media_video){100, 4000000, 12, 0, 12, STREAM_MEDIA_KEY, expected_video};
    CHECK(media_fixture_feed(&f, &b) == STREAM_OK);
    CHECK(stream_media_store_recovery(f.store, &position) == STREAM_OK && position.key_valid && position.key_seq == f.seq - 1);
    stream_media_store_gap(f.store, f.seq, f.seq);
    CHECK(stream_media_store_recovery(f.store, &position) == STREAM_OK && !position.key_valid && position.last_seq == f.seq);
    media_fixture_close(&f);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS recovery positions: complete/fragmented KEY, retained previous KEY, eviction and GAP");
}

int main(int argc, char **argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    /* Усечения проверяются массово; ожидаемые отказы не загромождают диагностический лог. */
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_DISABLED);
    size_t before = u_get_allocated_count();
    for (unsigned i = 0; i < sizeof(expected_video); i++) expected_video[i] = (uint8_t)(i * 31 + 7);
    if (argc == 2 && !strcmp(argv[1], "--revision-transition")) {
        debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_DEBUG);
        test_revision_transition(); test_cursor_key_recovery();
    } else {
        CHECK(argc == 1);
        test_media(); test_batches(); test_stereo(); test_codecs();
        debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
        test_revision_transition(); test_cursor_key_recovery(); test_format_conflicts(); test_recovery_positions();
    }
    CHECK(u_get_allocated_count() == before);
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS stream_media: protocol, FEC, frame recovery, boundaries, no leaks");
    return 0;
}
