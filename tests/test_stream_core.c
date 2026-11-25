#include "stream_address_fixture.h"
/* Подпись источника, RS-потери, кольцо, циклическое время и реальные uasync-таймеры источника. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/stream/stream_source.h"
#include "../src/stream/stream_metrics.h"
#include "../src/stream/stream_load.h"
#include "../src/stream/stream_optimizer.h"
#include "../src/stream/stream_network.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_clock.h"

#define CHECK(expr) do { if (!(expr)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "test_stream_core failed line=%d: %s", __LINE__, #expr); exit(1); \
} } while (0)

static struct stream_descriptor descriptor = {
    .group_id = UINT64_C(0x0123456789abcdef), .group_key = {1, 2, [31] = 3},
    .source_epoch = UINT64_C(0x0102030405060708), .block_size = STREAM_RS_DEFAULT_SIZE, .n = 8, .r = 2
};
/* Стенд потерь использует 6+2 независимо от настроек публикации по умолчанию. */
#define TEST_INPUT_CAPACITY (6 * (STREAM_RS_DEFAULT_SIZE - RS_HEADER_SIZE) - STREAM_BATCH_OVERHEAD)
static uint8_t private_key[32];
static unsigned cache_added, cache_removed;
static void cache_observer(void *arg, const uint8_t *wire, size_t length, int added) {
    (void)arg; CHECK(length > STREAM_BLOCK_OVERHEAD + RS_HEADER_SIZE && length <= STREAM_BLOCK_DEFAULT_SIZE
                    && !memcmp(wire + STREAM_BLOCK_GROUP_OFF, descriptor.group_key, SC_PUBKEY_SIZE));
    if (added) cache_added++; else cache_removed++;
}

/* Рост до X, ровно 500мс подтверждения, короткий глитч, смена состава и фактические задержанные интервалы. */
static void test_load(void) {
    struct stream_load load; struct stream_load_snapshot snapshot;
    stream_load_init(&load, 0);
    CHECK(stream_load_add(&load, 10, 10000) == STREAM_OK);
    CHECK(stream_load_remove(&load, 6, 6000, 1) == STREAM_OK);
    CHECK(stream_load_poll(&load, 200, 250, &snapshot) == STREAM_OK && snapshot.growth_rate == 20000 && !snapshot.confirmed);
    CHECK(stream_load_add(&load, 2, 2000) == STREAM_OK && stream_load_remove(&load, 1, 1000, 0) == STREAM_OK);
    CHECK(stream_load_poll(&load, 400, 450, &snapshot) == STREAM_OK && snapshot.growth_rate == 15000);
    CHECK(snapshot.queued.bytes == 5000 && snapshot.arrivals.bytes == snapshot.sent.bytes + snapshot.removed.bytes + snapshot.queued.bytes);
    CHECK(stream_load_poll(&load, 699, 749, &snapshot) == STREAM_OK && !snapshot.confirmed);
    CHECK(stream_load_poll(&load, 700, 750, &snapshot) == STREAM_WAIT && snapshot.confirmed);
    CHECK(stream_load_remove(&load, 5, 5000, 0) == STREAM_OK);
    CHECK(stream_load_poll(&load, 701, 0, &snapshot) == STREAM_WAIT && !snapshot.pressure && !snapshot.confirmed);
    CHECK(stream_load_remove(&load, 1, 1000, 1) == STREAM_ERR_STATE);
    stream_load_init(&load, 1000);
    CHECK(stream_load_add(&load, 2, UINT64_C(1) << 40) == STREAM_OK);
    CHECK(stream_load_poll(&load, 1300, 300, &snapshot) == STREAM_OK && !snapshot.confirmed && snapshot.interval_ms == 300);
    CHECK(stream_load_remove(&load, 2, UINT64_C(1) << 40, 1) == STREAM_OK);
    CHECK(stream_load_poll(&load, 1400, 0, &snapshot) == STREAM_WAIT && !snapshot.confirmed);
    CHECK(stream_load_poll(&load, 1500, 0, &snapshot) == STREAM_OK && snapshot.arrivals.bytes == snapshot.sent.bytes);
    CHECK(stream_load_add(&load, 3, 3900) == STREAM_OK); stream_load_changed(&load);
    CHECK(stream_load_poll(&load, 1700, 300, &snapshot) == STREAM_OK && snapshot.growth_rate == 0 && !snapshot.confirmed);
    CHECK(stream_load_poll(&load, 13000, 300, &snapshot) == STREAM_OK && snapshot.input_rate == 0);
    struct stream_metric_snapshot input = {.lag_ms = UINT64_C(1) << 42, .blocks = UINT64_MAX, .late_blocks = 2, .stops = 3,
        .source_rate_bytes_sec = UINT64_C(1) << 40, .receive_rate_bytes_sec = 10000, .source_rate_valid = 1, .receive_rate_valid = 1, .stopped = 1};
    uint8_t wire[STREAM_METRIC_WIRE_SIZE]; stream_metric_write(wire, &input, 1);
    struct stream_metric_snapshot output; int valid = 0;
    CHECK(stream_metric_read(wire, &output, &valid) == STREAM_OK && valid && output.blocks == UINT64_MAX
          && output.lag_ms == input.lag_ms && output.source_rate_bytes_sec == input.source_rate_bytes_sec && output.stopped);
    wire[0] |= 128; CHECK(stream_metric_read(wire, &output, &valid) == STREAM_ERR_FORMAT);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] load: A/D/X/Q, growth before drops, 500ms boundary, glitches, revisions, interval/window and wide metrics");
}

/* Воспроизводимые данные, не совпадающие с zero-padding и проверочными кодограммами. */
static void pattern(uint8_t *data, size_t length) {
    uint32_t state = 0x12345678;
    for (size_t i = 0; i < length; i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        data[i] = (uint8_t)state;
    }
}

/* Подпись находится внутри RS; внешние поля проверяются структурно, подмена обнаруживается после восстановления. */
static void test_signed_blocks(void) {
    struct stream_encoder *encoder = NULL;
    CHECK(stream_encoder_create(&encoder, &descriptor, private_key) == STREAM_OK);
    CHECK(stream_block_capacity(&descriptor) == 1300 && stream_input_size(&descriptor) == TEST_INPUT_CAPACITY);
    uint8_t input[TEST_INPUT_CAPACITY]; pattern(input, sizeof(input));
    const uint8_t *blocks[RS_MAX_BLOCKS];
    uint64_t seq = UINT64_C(0x1122334455667788);
    size_t size = 0;
    CHECK(stream_encoder_encode(encoder, seq, 65530, STREAM_BLOCK_TIME_RESET, input, sizeof(input), blocks, &size) == STREAM_OK);
    CHECK(size == 1300);
    const uint8_t *public_key = stream_encoder_public_key(encoder);
    struct stream_block_view view;
    const uint8_t *received[RS_MAX_BLOCKS] = {0};
    for (unsigned i = 0; i < descriptor.n; i++) {
        CHECK(stream_block_parse(&descriptor, blocks[i], size, &view) == STREAM_OK);
        CHECK(view.batch.seq == seq && view.subblock == i && view.batch.timestamp_ms == 65530 && view.batch.data_length == sizeof(input));
        CHECK(view.batch.flags == STREAM_BLOCK_TIME_RESET && view.rs == blocks[i] + STREAM_BLOCK_RS_OFF);
        CHECK(view.batch.block_size == descriptor.block_size);
        CHECK(!memcmp(blocks[i], STREAM_BLOCK_DOMAIN, 12));
        CHECK(!memcmp(blocks[i] + STREAM_BLOCK_GROUP_OFF, descriptor.group_key, SC_PUBKEY_SIZE));
        CHECK(!memcmp(blocks[i] + STREAM_BLOCK_SEQ_OFF, "\x11\x22\x33\x44\x55\x66\x77\x88", 8));
        received[i] = view.rs;
    }
    struct rs_codec *rs = NULL;
    CHECK(rs_init(&rs, descriptor.n, descriptor.r, descriptor.block_size) == RS_OK);
    uint8_t decoded[TEST_INPUT_CAPACITY + STREAM_BATCH_OVERHEAD];
    CHECK(rs_decode(rs, received, descriptor.block_size, decoded, sizeof(decoded)) == RS_OK);
    CHECK(!memcmp(decoded, STREAM_BATCH_DOMAIN, 12));
    CHECK(!memcmp(decoded + STREAM_BATCH_HEADER, input, sizeof(input)));
    CHECK(stream_batch_verify(&descriptor, public_key, &view.batch, decoded, sizeof(decoded)) == STREAM_OK);
    CHECK(sc_ed25519_verify(public_key, decoded, STREAM_BATCH_HEADER + sizeof(input),
                            decoded + STREAM_BATCH_HEADER + sizeof(input)) == SC_OK);
    const unsigned changed[] = {0, STREAM_BLOCK_GROUP_OFF, STREAM_BLOCK_GROUP_OFF + SC_PUBKEY_SIZE - 1,
        STREAM_BLOCK_SOURCE_OFF, STREAM_BLOCK_EPOCH_OFF, STREAM_BLOCK_N_OFF, STREAM_BLOCK_R_OFF, STREAM_BLOCK_SIZE_OFF};
    uint8_t wire[STREAM_BLOCK_MAX_SIZE];
    for (unsigned i = 0; i < sizeof(changed) / sizeof(changed[0]); i++) {
        memcpy(wire, blocks[0], size); wire[changed[i]] ^= 1;
        struct stream_block_view parsed; memset(&parsed, 0xa5, sizeof(parsed));
        uint8_t before[sizeof(parsed)]; memcpy(before, &parsed, sizeof(parsed));
        CHECK(stream_block_parse(&descriptor, wire, size, &parsed) < 0);
        CHECK(!memcmp(before, &parsed, sizeof(parsed)));
    }
    CHECK(stream_block_parse(&descriptor, blocks[0], 0, &view) == STREAM_ERR_FORMAT);
    CHECK(stream_block_parse(&descriptor, blocks[0], size - 1, &view) == STREAM_ERR_FORMAT);
    CHECK(stream_block_parse(&descriptor, blocks[0], size + 1, &view) == STREAM_ERR_FORMAT);
    uint8_t wrong_key[32]; memcpy(wrong_key, public_key, 32); wrong_key[0] ^= 1;
    CHECK(stream_batch_verify(&descriptor, wrong_key, &view.batch, decoded, sizeof(decoded)) == STREAM_ERR_SIGNATURE);
    struct stream_descriptor wrong_group = descriptor; wrong_group.group_key[SC_PUBKEY_SIZE - 1] ^= 1;
    CHECK(stream_batch_verify(&wrong_group, public_key, &view.batch, decoded, sizeof(decoded)) == STREAM_ERR_FORMAT);
    const unsigned signed_fields[] = {0, STREAM_BLOCK_GROUP_OFF, STREAM_BLOCK_GROUP_OFF + SC_PUBKEY_SIZE - 1,
        STREAM_BLOCK_SOURCE_OFF, STREAM_BLOCK_EPOCH_OFF, STREAM_BLOCK_N_OFF, STREAM_BLOCK_R_OFF, STREAM_BLOCK_SIZE_OFF,
        STREAM_BLOCK_TIMESTAMP_OFF, STREAM_BLOCK_LENGTH_OFF, STREAM_BLOCK_FLAGS_OFF, STREAM_BATCH_SEQ_OFF};
    uint8_t modified[sizeof(decoded)];
    for (unsigned i = 0; i < sizeof(signed_fields) / sizeof(signed_fields[0]); i++) {
        memcpy(modified, decoded, sizeof(decoded)); modified[signed_fields[i]] ^= 1;
        CHECK(stream_batch_verify(&descriptor, public_key, &view.batch, modified, sizeof(modified)) == STREAM_ERR_SIGNATURE);
        CHECK(sc_ed25519_sign(private_key, modified, STREAM_BATCH_HEADER + sizeof(input),
                             modified + STREAM_BATCH_HEADER + sizeof(input)) == SC_OK);
        CHECK(stream_batch_verify(&descriptor, public_key, &view.batch, modified, sizeof(modified)) == STREAM_ERR_FORMAT);
    }
    memcpy(wire, blocks[0], size); wire[STREAM_BLOCK_RS_OFF + RS_HEADER_SIZE + STREAM_BATCH_HEADER] ^= 1;
    CHECK(stream_block_parse(&descriptor, wire, size, &view) == STREAM_OK);
    received[0] = wire + STREAM_BLOCK_RS_OFF;
    CHECK(rs_decode(rs, received, descriptor.block_size, decoded, sizeof(decoded)) == RS_OK);
    CHECK(stream_batch_verify(&descriptor, public_key, &view.batch, decoded, sizeof(decoded)) == STREAM_ERR_SIGNATURE);
    rs_free(rs);
    size = 999;
    CHECK(stream_encoder_encode(encoder, UINT64_MAX, 0, 0, input, 1, blocks, &size) == STREAM_ERR_ARGUMENT);
    CHECK(stream_encoder_encode(encoder, 1, 0, 0, input, 0, blocks, &size) == STREAM_ERR_ARGUMENT);
    CHECK(stream_encoder_encode(encoder, 1, 0, 2, input, 1, blocks, &size) == STREAM_ERR_ARGUMENT);
    CHECK(stream_encoder_encode(encoder, 1, 0, 0, input, sizeof(input) + 1, blocks, &size) == STREAM_ERR_ARGUMENT && size == 999);
    struct stream_descriptor invalid = descriptor; invalid.n = 0;
    CHECK(stream_descriptor_validate(&invalid) == STREAM_ERR_ARGUMENT);
    invalid = descriptor; invalid.r = invalid.n;
    CHECK(stream_descriptor_validate(&invalid) == STREAM_ERR_ARGUMENT);
    invalid = descriptor; invalid.block_size = 9;
    CHECK(stream_descriptor_validate(&invalid) == STREAM_ERR_ARGUMENT);
    invalid = descriptor; invalid.n = 1; invalid.r = 0; invalid.block_size = RS_HEADER_SIZE + STREAM_BATCH_OVERHEAD;
    CHECK(stream_descriptor_validate(&invalid) == STREAM_ERR_ARGUMENT);
    invalid = descriptor; invalid.source_epoch = 0;
    CHECK(stream_descriptor_validate(&invalid) == STREAM_ERR_ARGUMENT);
    stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] signed RS batch: canonical bytes, payload/signature recovery, signed metadata and size validation");
}

/* Wire сохраняет все 64 бита и не меняет выход при усечении; путь не допускает повторного посещения. */
static void test_wire(void) {
    uint8_t wire[STREAM_WIRE_HEADER + 8];
    struct stream_message input = {.descriptor = descriptor, .sender_epoch = UINT64_MAX, .receiver_epoch = UINT64_MAX - 1,
        .handle = UINT64_MAX - 2, .command = STREAM_CMD_SUBSCRIBE, .subblock = 7};
    stream_wire_header(wire, &input); stream_write_be(wire + STREAM_WIRE_HEADER, UINT64_MAX - 3, 8);
    struct stream_message parsed;
    CHECK(stream_wire_parse(wire, sizeof(wire), &parsed) == STREAM_OK);
    CHECK(parsed.sender_epoch == input.sender_epoch && parsed.receiver_epoch == input.receiver_epoch && parsed.handle == input.handle);
    CHECK(!memcmp(parsed.descriptor.group_key, descriptor.group_key, SC_PUBKEY_SIZE) && parsed.descriptor.source_epoch == descriptor.source_epoch);
    CHECK(parsed.subblock == 7 && parsed.length == 8 && stream_read_be(parsed.payload, 8) == UINT64_MAX - 3);
    for (size_t i = 0; i < STREAM_WIRE_HEADER; i++) {
        memset(&parsed, 0xa5, sizeof(parsed)); uint8_t before[sizeof(parsed)]; memcpy(before, &parsed, sizeof(parsed));
        CHECK(stream_wire_parse(wire, i, &parsed) == STREAM_ERR_FORMAT && !memcmp(before, &parsed, sizeof(parsed)));
    }
    wire[1]++; CHECK(stream_wire_parse(wire, sizeof(wire), &parsed) == STREAM_ERR_FORMAT); wire[1]--;
    wire[2] = 255; CHECK(stream_wire_parse(wire, sizeof(wire), &parsed) == STREAM_ERR_FORMAT);
    struct stream_path path = {.nodes = {descriptor.source, STREAM_TEST_ADDR(1), STREAM_TEST_ADDR(2)}, .count = 3}, read;
    uint8_t encoded[1 + STREAM_PATH_MAX * SC_PUBKEY_SIZE]; size_t used = 0, length = stream_path_write(encoded, &path);
    CHECK(stream_path_parse(encoded, length, &read, &used) == STREAM_OK && used == length && read.count == 3 && stream_address_equal(read.nodes[2], STREAM_TEST_ADDR(2)));
    CHECK(stream_path_validate(&read, descriptor.source, STREAM_TEST_ADDR(2), STREAM_TEST_ADDR(3)) == STREAM_OK);
    CHECK(stream_path_validate(&read, descriptor.source, STREAM_TEST_ADDR(2), STREAM_TEST_ADDR(1)) == STREAM_ERR_FORMAT);
    read.nodes[2] = STREAM_TEST_ADDR(1); CHECK(stream_path_validate(&read, descriptor.source, STREAM_TEST_ADDR(1), STREAM_TEST_ADDR(0)) == STREAM_ERR_FORMAT);
    for (size_t i = 0; i < length; i++) CHECK(stream_path_parse(encoded, i, &read, &used) == STREAM_ERR_FORMAT);
    encoded[0] = STREAM_PATH_MAX + 1; CHECK(stream_path_parse(encoded, sizeof(encoded), &read, &used) == STREAM_ERR_FORMAT);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] stream wire: 64-bit fields, truncation, version, path repeats/receiver loop and size");
}

/* Любая пара потерь восстанавливает содержимое и общую подпись, отсутствующие части не регенерируются. */
static void test_fec_and_padding(void) {
    struct stream_encoder *encoder = NULL;
    CHECK(stream_encoder_create(&encoder, &descriptor, private_key) == STREAM_OK);
    uint8_t input[TEST_INPUT_CAPACITY], output[TEST_INPUT_CAPACITY]; pattern(input, sizeof(input));
    const uint8_t *blocks[RS_MAX_BLOCKS];
    size_t size = 0;
    CHECK(stream_encoder_encode(encoder, 0, 15, 0, input, sizeof(input), blocks, &size) == STREAM_OK && size == 1300);
    for (unsigned first = 0; first < 8; first++) {
        for (unsigned second = first + 1; second < 8; second++) {
            struct stream_buffer *buffer = NULL;
            CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 8 * 1300) == STREAM_OK);
            for (unsigned i = 0; i < 8; i++)
                if (i != first && i != second) CHECK(stream_buffer_put(buffer, blocks[i], size) == STREAM_OK);
            size_t length = 0;
            memset(output, 0xcc, sizeof(output));
            size_t allocations = u_get_allocated_count();
            CHECK(stream_buffer_decode(buffer, 0, output, sizeof(output), &length) == STREAM_OK);
            CHECK(u_get_allocated_count() == allocations && length == sizeof(input) && !memcmp(input, output, length));
            const uint8_t *missing = NULL; size_t missing_length = 0; uint64_t actual = 100;
            CHECK(stream_buffer_read(buffer, 0, (uint8_t)first, &missing, &missing_length, &actual) == STREAM_GAP);
            CHECK(!missing && missing_length == 0 && actual == 100);
            struct stream_buffer_stats stats; CHECK(stream_buffer_get_stats(buffer, &stats) == STREAM_OK);
            CHECK(stats.blocks == 6 && stats.stored_bytes == 6 * 1300);
            stream_buffer_destroy(buffer);
        }
    }
    struct stream_buffer *buffer = NULL;
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 8 * 1300) == STREAM_OK);
    for (unsigned i = 0; i < 5; i++) CHECK(stream_buffer_put(buffer, blocks[i], size) == STREAM_OK);
    memset(output, 0xcc, sizeof(output)); size_t length = 123;
    CHECK(stream_buffer_decode(buffer, 0, output, sizeof(output), &length) == STREAM_WAIT);
    CHECK(length == 123);
    for (unsigned i = 0; i < sizeof(output); i++) CHECK(output[i] == 0xcc);
    CHECK(stream_buffer_put(buffer, blocks[5], size) == STREAM_OK);
    CHECK(stream_buffer_decode(buffer, 0, output, 1, &length) == STREAM_ERR_ARGUMENT);
    stream_buffer_destroy(buffer);
    stream_encoder_destroy(encoder);

    const struct { uint8_t n, r; uint16_t size; } variants[] = {{1, 0, 1189}, {3, 1, RS_HEADER_SIZE + STREAM_BATCH_OVERHEAD / 2 + 2}, {16, 5, 4096}, {255, 254, STREAM_BATCH_OVERHEAD + RS_HEADER_SIZE + 2}};
    for (unsigned v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        struct stream_descriptor d = descriptor; d.n = variants[v].n; d.r = variants[v].r; d.block_size = variants[v].size;
        encoder = NULL; buffer = NULL;
        CHECK(stream_encoder_create(&encoder, &d, private_key) == STREAM_OK);
        size_t size = 0, wanted = stream_input_size(&d) > 17 ? 17 : stream_input_size(&d);
        CHECK(stream_encoder_encode(encoder, 37, 0, 0, input, wanted, blocks, &size) == STREAM_OK);
        CHECK(stream_buffer_create(&buffer, &d, stream_encoder_public_key(encoder), d.n * stream_block_capacity(&d)) == STREAM_OK);
        for (unsigned i = d.r; i < d.n; i++) CHECK(stream_buffer_put(buffer, blocks[i], size) == STREAM_OK);
        uint8_t partial[17]; length = 0;
        CHECK(stream_buffer_decode(buffer, 37, partial, wanted, &length) == STREAM_OK);
        CHECK(length == wanted && !memcmp(partial, input, wanted));
        stream_buffer_destroy(buffer); stream_encoder_destroy(encoder);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] FEC: all 28 double losses, insufficient set, partial data, n=1..255 and r=0");
}

/* Размеры чередуются в одном кольце; каждая пачка восстанавливается без двух исходных частей. */
static void test_variable_batches(void) {
    struct stream_encoder *encoder = NULL; struct stream_buffer *buffer = NULL;
    CHECK(stream_encoder_create(&encoder, &descriptor, private_key) == STREAM_OK);
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 64 * 1300) == STREAM_OK);
    uint8_t input[TEST_INPUT_CAPACITY], output[TEST_INPUT_CAPACITY]; pattern(input, sizeof(input));
    const size_t lengths[] = {1, 7, sizeof(input), 17, 1000, 6, 2};
    size_t sizes[sizeof(lengths) / sizeof(lengths[0])], stored = 0;
    const uint8_t *blocks[RS_MAX_BLOCKS];
    for (unsigned seq = 0; seq < sizeof(lengths) / sizeof(lengths[0]); seq++) {
        CHECK(stream_encoder_encode(encoder, seq, (uint16_t)seq, 0, input, lengths[seq], blocks, &sizes[seq]) == STREAM_OK);
        CHECK(sizes[seq] == STREAM_BLOCK_OVERHEAD + RS_HEADER_SIZE + (STREAM_BATCH_OVERHEAD + lengths[seq] + 5) / 6);
        for (unsigned sub = 8; sub-- > 2;) CHECK(stream_buffer_put(buffer, blocks[sub], sizes[seq]) == STREAM_OK);
        stored += 6 * sizes[seq];
    }
    for (unsigned seq = 0; seq < sizeof(lengths) / sizeof(lengths[0]); seq++) {
        size_t length = 999; memset(output, 0xcc, sizeof(output));
        CHECK(stream_buffer_decode(buffer, seq, output, sizeof(output), &length) == STREAM_OK);
        CHECK(length == lengths[seq] && !memcmp(input, output, length));
        for (size_t i = length; i < sizeof(output); i++) CHECK(output[i] == 0xcc);
        const uint8_t *wire; uint64_t actual;
        CHECK(stream_buffer_read(buffer, seq, 2, &wire, &length, &actual) == STREAM_OK && length == sizes[seq] && actual == seq);
    }
    uint64_t count = 0, bytes = 0;
    CHECK(stream_buffer_pending(buffer, 0, 2, &count, &bytes, NULL) == STREAM_OK);
    CHECK(count == sizeof(lengths) / sizeof(lengths[0]) && bytes == stored / 6);
    struct stream_buffer_stats stats;
    CHECK(stream_buffer_get_stats(buffer, &stats) == STREAM_OK && stats.stored_bytes == stored && !stats.rejected_batches);
    stream_buffer_destroy(buffer); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] variable batches: tiny/full/odd lengths, two source losses, exact output, actual cache and queue bytes");
}

/* Повреждение содержимого/подписи и подмена внешних подсказок дают отказ после RS; следующая пачка проходит. */
static void test_rejected_batches(void) {
    struct stream_encoder *encoder = NULL;
    CHECK(stream_encoder_create(&encoder, &descriptor, private_key) == STREAM_OK);
    uint8_t input[1000], output[1000], parts[8][1300]; pattern(input, sizeof(input));
    const uint8_t *blocks[RS_MAX_BLOCKS];
    for (unsigned variant = 0; variant < 5; variant++) {
        struct stream_buffer *buffer = NULL;
        uint8_t key[32]; memcpy(key, stream_encoder_public_key(encoder), sizeof(key));
        if (variant == 4) key[0] ^= 1;
        CHECK(stream_buffer_create(&buffer, &descriptor, key, 16 * 1300) == STREAM_OK);
        size_t size = 0;
        CHECK(stream_encoder_encode(encoder, 10, 42, 0, input, sizeof(input), blocks, &size) == STREAM_OK);
        size_t payload = size - STREAM_BLOCK_OVERHEAD - RS_HEADER_SIZE;
        for (unsigned sub = 0; sub < 8; sub++) memcpy(parts[sub], blocks[sub], size);
        if (variant < 2) {
            size_t offset = STREAM_BATCH_HEADER + (variant == 1 ? sizeof(input) : 0);
            parts[offset / payload][STREAM_BLOCK_RS_OFF + RS_HEADER_SIZE + offset % payload] ^= 1;
        } else if (variant == 2) {
            for (unsigned sub = 0; sub < 8; sub++) parts[sub][STREAM_BLOCK_TIMESTAMP_OFF] ^= 1;
        } else if (variant == 3) {
            for (unsigned sub = 0; sub < 8; sub++) stream_write_be(parts[sub] + STREAM_BLOCK_SEQ_OFF, 10 + 1, 8);
        }
        uint64_t seq = variant == 3 ? 11 : 10;
        for (unsigned sub = 0; sub < 6; sub++) CHECK(stream_buffer_put(buffer, parts[sub], size) == STREAM_OK);
        size_t length = 999; memset(output, 0xcc, sizeof(output));
        CHECK(stream_buffer_decode(buffer, seq, output, sizeof(output), &length) == STREAM_REJECTED && length == 999);
        CHECK(stream_buffer_decode(buffer, seq, output, sizeof(output), &length) == STREAM_REJECTED);
        for (unsigned i = 0; i < sizeof(output); i++) CHECK(output[i] == 0xcc);
        struct stream_buffer_stats stats;
        CHECK(stream_buffer_get_stats(buffer, &stats) == STREAM_OK && stats.rejected_batches == 1);
        if (variant != 4) {
            CHECK(stream_encoder_encode(encoder, seq + 1, 43, 0, input, 17, blocks, &size) == STREAM_OK);
            for (unsigned sub = 0; sub < 6; sub++) CHECK(stream_buffer_put(buffer, blocks[sub], size) == STREAM_OK);
            CHECK(stream_buffer_decode(buffer, seq + 1, output, sizeof(output), &length) == STREAM_OK);
            CHECK(length == 17 && !memcmp(input, output, length));
        }
        stream_buffer_destroy(buffer);
    }
    /* Изменение padding не меняет подписанное тело, но нарушает канонический формат пачки. */
    struct rs_codec *rs = NULL; size_t size = 0;
    CHECK(rs_init(&rs, descriptor.n, descriptor.r, descriptor.block_size) == RS_OK);
    CHECK(stream_encoder_encode(encoder, 20, 42, 0, input, 17, blocks, &size) == STREAM_OK);
    const uint8_t *received[RS_MAX_BLOCKS] = {0};
    for (unsigned sub = 0; sub < 8; sub++) received[sub] = blocks[sub] + STREAM_BLOCK_RS_OFF;
    uint8_t decoded[TEST_INPUT_CAPACITY + STREAM_BATCH_OVERHEAD];
    size_t rs_size = size - STREAM_BLOCK_OVERHEAD, decoded_size = 6 * (rs_size - RS_HEADER_SIZE);
    CHECK(rs_decode(rs, received, rs_size, decoded, sizeof(decoded)) == RS_OK);
    struct stream_block_view view; CHECK(stream_block_parse(&descriptor, blocks[0], size, &view) == STREAM_OK);
    CHECK(stream_batch_verify(&descriptor, stream_encoder_public_key(encoder), &view.batch, decoded, decoded_size) == STREAM_OK);
    CHECK(decoded_size > STREAM_BATCH_OVERHEAD + 17);
    decoded[decoded_size - 1] = 1;
    CHECK(stream_batch_verify(&descriptor, stream_encoder_public_key(encoder), &view.batch, decoded, decoded_size) == STREAM_ERR_FORMAT);
    rs_free(rs); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] rejected batches: payload/signature, signed time/index, wrong key, padding, unchanged output and recovery");
}

/* Сохранять части подписанной пачки до повторного использования буферов encoder. */
static void put_batch(struct stream_encoder *encoder, struct stream_buffer *buffer, uint64_t seq, unsigned mask) {
    uint8_t input[37]; pattern(input, sizeof(input));
    const uint8_t *blocks[RS_MAX_BLOCKS];
    size_t size = 0;
    CHECK(stream_encoder_encode(encoder, seq, (uint16_t)seq, 0, input, sizeof(input), blocks, &size) == STREAM_OK);
    for (unsigned i = 0; i < 8; i++) if (mask & (1u << i)) CHECK(stream_buffer_put(buffer, blocks[i], size) == STREAM_OK);
}

/* Общее кольцо хранит реальные блоки, принимает порядок 10/12/11 и вытесняет целую минимальную пачку. */
static void test_ring_and_conflicts(void) {
    struct stream_encoder *encoder = NULL; struct stream_buffer *buffer = NULL;
    CHECK(stream_encoder_create(&encoder, &descriptor, private_key) == STREAM_OK);
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 16 * 1300) == STREAM_OK);
    cache_added = cache_removed = 0; stream_buffer_set_observer(buffer, cache_observer, NULL);
    const uint8_t *wire = NULL; size_t length = 0; uint64_t actual = 100;
    CHECK(stream_buffer_read(buffer, UINT64_MAX, 0, &wire, &length, &actual) == STREAM_WAIT);
    put_batch(encoder, buffer, 10, 255); put_batch(encoder, buffer, 12, 255);
    CHECK(stream_buffer_read(buffer, 10, 0, &wire, &length, &actual) == STREAM_OK);
    CHECK(stream_buffer_put(buffer, wire, length) == STREAM_DUPLICATE);
    uint8_t bad[1300]; memcpy(bad, wire, length); bad[STREAM_BLOCK_RS_OFF + RS_HEADER_SIZE] ^= 1;
    CHECK(stream_buffer_put(buffer, bad, length) == STREAM_ERR_CONFLICT);
    put_batch(encoder, buffer, 11, 1);
    CHECK(cache_added == 17 && cache_removed == 8);
    uint64_t pending = 0, oldest = 0;
    uint64_t bytes = 0;
    CHECK(stream_buffer_pending(buffer, 11, 0, &pending, &bytes, &oldest) == STREAM_OK && pending == 2 && bytes == 2 * length && oldest);
    CHECK(stream_buffer_pending(buffer, 12, 0, &pending, NULL, &oldest) == STREAM_OK && pending == 1);
    CHECK(stream_buffer_read(buffer, 10, 0, &wire, &length, &actual) == STREAM_EXPIRED);
    CHECK(stream_buffer_read(buffer, 11, 1, &wire, &length, &actual) == STREAM_GAP);
    CHECK(stream_buffer_read(buffer, 13, 0, &wire, &length, &actual) == STREAM_WAIT);
    CHECK(stream_buffer_read(buffer, UINT64_MAX, 0, &wire, &length, &actual) == STREAM_OK && actual == 12);
    uint64_t first = 0, last = 0;
    CHECK(stream_buffer_range(buffer, 0, &first, &last) == STREAM_OK && first == 11 && last == 12);
    struct stream_buffer_stats stats; CHECK(stream_buffer_get_stats(buffer, &stats) == STREAM_OK);
    CHECK(stats.blocks == 9 && stats.batches == 2 && stats.stored_bytes == 9 * length && stats.expired_before == 11);
    const uint8_t *blocks[RS_MAX_BLOCKS]; uint8_t input[37]; pattern(input, sizeof(input));
    CHECK(stream_encoder_encode(encoder, 10, 10, 0, input, sizeof(input), blocks, &length) == STREAM_OK);
    CHECK(stream_buffer_put(buffer, blocks[0], length) == STREAM_EXPIRED);
    put_batch(encoder, buffer, 13, 255);
    CHECK(stream_buffer_read(buffer, 11, 0, &wire, &length, &actual) == STREAM_EXPIRED);
    put_batch(encoder, buffer, UINT64_MAX - 1, 1);
    CHECK(stream_buffer_read(buffer, UINT64_MAX, 0, &wire, &length, &actual) == STREAM_OK && actual == UINT64_MAX - 1);
    CHECK(stream_buffer_read(buffer, UINT64_MAX, 7, &wire, &length, &actual) == STREAM_OK && actual == 13);
    CHECK(stream_buffer_get_stats(buffer, &stats) == STREAM_OK && stats.stored_bytes <= stats.capacity_bytes);
    stream_buffer_destroy(buffer);

    buffer = NULL;
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 16 * 1300) == STREAM_OK);
    for (unsigned seq = 0; seq < 16; seq++) put_batch(encoder, buffer, seq, 1);
    CHECK(stream_buffer_get_stats(buffer, &stats) == STREAM_OK && stats.blocks == 16 && stats.batches == 16);
    CHECK(stats.stored_bytes == 16 * length && stats.stored_bytes < stats.capacity_bytes); /* Ёмкость слотов и длины данных различаются. */
    put_batch(encoder, buffer, 16, 1);
    CHECK(stream_buffer_read(buffer, 0, 0, &wire, &length, &actual) == STREAM_EXPIRED);
    CHECK(stream_buffer_range(buffer, 0, &first, &last) == STREAM_OK && first == 1 && last == 16);
    stream_buffer_destroy(buffer); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] ring: shared data budget, sparse batches, out-of-order indices, whole-batch eviction and conflicts");
}

/* Подлинное время источника разворачивается через ноль; постоянный lag не считается полной остановкой. */
static void test_relative_metrics(void) {
    struct stream_metrics m; struct stream_metric_snapshot snapshot;
    stream_metrics_init(&m);
    CHECK(stream_metrics_poll(&m, 0, &snapshot) == STREAM_WAIT);
    CHECK(stream_metrics_observe(&m, 100, 65520, 0, 1300, 1000) == STREAM_OK);
    CHECK(stream_metrics_observe(&m, 101, 4, 0, 1300, 1020) == STREAM_OK);
    CHECK(stream_metrics_poll(&m, 1020, &snapshot) == STREAM_OK);
    CHECK(snapshot.lag_ms == 0 && snapshot.source_rate_bytes_sec == 65000 && snapshot.expected_interval_ms == 20);
    CHECK(snapshot.receive_rate_bytes_sec == 65000 && snapshot.blocks == 2 && !snapshot.stopped);
    CHECK(stream_metrics_observe(&m, 102, 24, 0, 1300, 1320) == STREAM_OK);
    CHECK(stream_metrics_observe(&m, 103, 44, 0, 1300, 1620) == STREAM_OK);
    CHECK(stream_metrics_poll(&m, 1620, &snapshot) == STREAM_OK);
    CHECK(snapshot.lag_ms == 560 && snapshot.late_blocks == 1 && snapshot.stops == 0 && !snapshot.stopped);
    uint64_t seq = 103, now = 1620; uint16_t timestamp = 44;
    for (unsigned i = 0; i < 30; i++) {
        seq++; now++; timestamp = (uint16_t)(timestamp + 20);
        CHECK(stream_metrics_observe(&m, seq, timestamp, 0, 1300, now) == STREAM_OK);
    }
    CHECK(stream_metrics_poll(&m, now, &snapshot) == STREAM_OK && snapshot.lag_ms == 0);
    CHECK(stream_metrics_observe(&m, seq, timestamp, 0, 1300, now + 300) == STREAM_DUPLICATE);
    CHECK(stream_metrics_poll(&m, now + 521, &snapshot) == STREAM_OK);
    CHECK(snapshot.stopped && snapshot.stops == 1 && snapshot.lag_ms == 501);
    CHECK(stream_metrics_poll(&m, now + 800, &snapshot) == STREAM_OK && snapshot.stops == 1);
    seq++; timestamp = (uint16_t)(timestamp + 20); now += 800;
    CHECK(stream_metrics_observe(&m, seq, timestamp, 0, 1300, now) == STREAM_OK);
    CHECK(stream_metrics_poll(&m, now, &snapshot) == STREAM_OK && !snapshot.stopped && snapshot.stops == 1);
    CHECK(stream_metrics_observe(&m, seq + 1, timestamp, 0, 1300, now - 1) == STREAM_ERR_STATE);
    stream_metrics_end(&m);
    CHECK(stream_metrics_poll(&m, now + 70000, &snapshot) == STREAM_OK);
    CHECK(!snapshot.blocks && !snapshot.late_blocks && !snapshot.stops && !snapshot.stopped && !snapshot.source_rate_valid);
    stream_metrics_init(&m);
    CHECK(stream_metrics_observe(&m, 0, 0, 0, 1300, 0) == STREAM_OK);
    CHECK(stream_metrics_observe(&m, 1, 0, 0, 1300, 0) == STREAM_OK);
    CHECK(stream_metrics_poll(&m, 0, &snapshot) == STREAM_OK && !snapshot.source_rate_valid);
    CHECK(stream_metrics_observe(&m, 2, 1, 0, 1300, 1) == STREAM_OK);
    CHECK(stream_metrics_poll(&m, 1, &snapshot) == STREAM_OK && snapshot.source_rate_bytes_sec == 2600000);
    CHECK(stream_metrics_observe(&m, 3, 9, STREAM_BLOCK_TIME_RESET, 1300, 2) == STREAM_OK);
    CHECK(stream_metrics_poll(&m, 2, &snapshot) == STREAM_OK && snapshot.lag_ms == 0);
    CHECK(stream_metrics_observe(&m, 5, 10, 0, 1300, 1000) == STREAM_OK);
    CHECK(stream_metrics_poll(&m, 1000, &snapshot) == STREAM_OK && snapshot.lag_ms == 0);
    CHECK(stream_metrics_observe(&m, 6, 11, 0, 1300, 71000) == STREAM_OK);
    CHECK(stream_metrics_poll(&m, 71000, &snapshot) == STREAM_OK && snapshot.lag_ms == 0 && !snapshot.stopped);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] metrics: timestamp wrap, zero deltas, lag/catchup, complete stall, one episode and window expiry");
}

struct source_events {
    unsigned count;
    uint64_t seq;
    uint64_t time_tb;
    uint16_t timestamp_ms;
    uint8_t flags;
};

/* Callback видит уже всю пачку, а не частично опубликованные подписи. */
static void source_ready(struct stream_source *source, uint64_t seq, void *arg) {
    struct source_events *events = arg;
    events->count++; events->seq = seq; events->time_tb = get_time_tb();
    struct stream_buffer_stats stats;
    CHECK(stream_buffer_get_stats(stream_source_buffer(source), &stats) == STREAM_OK);
    CHECK(stats.blocks == events->count * 8 && stats.highest_seq == seq);
    const uint8_t *wire = NULL; size_t length = 0; uint64_t actual = UINT64_MAX;
    CHECK(stream_buffer_read(stream_source_buffer(source), seq, 0, &wire, &length, &actual) == STREAM_OK);
    CHECK(actual == seq && length <= STREAM_BLOCK_DEFAULT_SIZE);
    events->timestamp_ms = (uint16_t)stream_read_be(wire + STREAM_BLOCK_TIMESTAMP_OFF, 2);
    events->flags = wire[STREAM_BLOCK_FLAGS_OFF];
    for (unsigned sub = 1; sub < 8; sub++) {
        CHECK(stream_buffer_read(stream_source_buffer(source), seq, (uint8_t)sub, &wire, &length, &actual) == STREAM_OK);
        CHECK(stream_read_be(wire + STREAM_BLOCK_TIMESTAMP_OFF, 2) == events->timestamp_ms);
        CHECK(wire[STREAM_BLOCK_FLAGS_OFF] == events->flags);
    }
}

/* Настоящий uasync timer: не сбрасывает до 100 мс, полный блок отменяет timer, destroy отменяет остаток. */
static void test_source_timer(void) {
    test_clock_init(50);
    struct UASYNC *ua = uasync_create(); CHECK(ua != NULL);
    struct stream_source *source = NULL; struct source_events events = {0};
    CHECK(stream_source_create(&source, ua, &descriptor, private_key, source_ready, &events) == STREAM_OK);
    uint8_t input[TEST_INPUT_CAPACITY], output[TEST_INPUT_CAPACITY]; pattern(input, sizeof(input));
    CHECK(stream_source_input_capacity(source) == sizeof(input));
    size_t accepted = 999;
    CHECK(stream_source_write(source, NULL, 0, &accepted) == STREAM_OK && accepted == 0);
    uint64_t start = get_time_tb();
    CHECK(stream_source_write(source, input, 17, &accepted) == STREAM_OK && accepted == 17);
    test_clock_run_for(ua, 500);
    CHECK(events.count == 0);
    test_clock_next_timer(ua);
    CHECK(events.count == 1 && events.seq == 0 && events.time_tb - start >= STREAM_SOURCE_FLUSH_TB);
    CHECK((uint16_t)(events.timestamp_ms - (uint16_t)(start / 10)) >= 100 && !events.flags);
    size_t length = 0;
    CHECK(stream_buffer_decode(stream_source_buffer(source), 0, output, 17, &length) == STREAM_OK);
    CHECK(length == 17 && !memcmp(input, output, length));
    CHECK(stream_source_write(source, input, sizeof(input), &accepted) == STREAM_OK && accepted == sizeof(input));
    CHECK(events.count == 2 && events.seq == 1);
    test_clock_run_for(ua, 1100);
    CHECK(events.count == 2); /* Нет лишней пустой пачки. */
    CHECK(stream_source_write(source, input, 13, &accepted) == STREAM_OK && accepted == 13);
    CHECK(stream_source_write(source, input + 13, sizeof(input) - 13, &accepted) == STREAM_OK);
    CHECK(events.count == 3 && events.seq == 2);
    test_clock_run_for(ua, 1100);
    CHECK(events.count == 3); /* Timer неполной пачки отменён заполнением. */
    CHECK(stream_buffer_decode(stream_source_buffer(source), 2, output, sizeof(output), &length) == STREAM_OK);
    CHECK(length == sizeof(input) && !memcmp(input, output, length));
    test_clock_run_for(ua, STREAM_TIMESTAMP_PERIOD_MS * 10);
    CHECK(stream_source_write(source, input, 23, &accepted) == STREAM_OK);
    CHECK(stream_source_finish(source) == STREAM_OK && events.count == 4);
    CHECK(events.flags == STREAM_BLOCK_TIME_RESET); /* Источник явно пометил неоднозначное число оборотов. */
    CHECK(stream_source_finish(source) == STREAM_OK && events.count == 4);
    CHECK(stream_source_write(source, input, 1, &accepted) == STREAM_ERR_STATE && accepted == 0);
    stream_source_destroy(source);
    source = NULL; events.count = 0;
    CHECK(stream_source_create(&source, ua, &descriptor, private_key, source_ready, &events) == STREAM_OK);
    CHECK(stream_source_write(source, input, 17, &accepted) == STREAM_OK);
    stream_source_destroy(source);
    test_clock_run_for(ua, 1100);
    CHECK(events.count == 0);
    uasync_destroy(ua, 1); test_clock_finish();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] source: 100ms timer, immediate full batch, padding, finish and cancellation on destroy");
}

/* Близкий сосед, цена дополнительных hops и проблем сравниваются в одних единицах, неизвестный RTT не становится нулём. */
static void test_supplier_score(void) {
    struct stream_receive_info near_peer = {.rtt_ms = 10, .path_rtt_ms = 80, .rtt_valid = 1, .path_rtt_valid = 1, .path = {.count = 3}};
    struct stream_receive_info far_peer = {.rtt_ms = 45, .path_rtt_ms = 40, .rtt_valid = 1, .path_rtt_valid = 1, .path = {.count = 2}};
    struct stream_metric_snapshot clean = {.blocks = 100}, late = {.blocks = 100, .late_blocks = 2};
    CHECK(stream_supplier_score(&near_peer, &clean, 1, 0) == 109 && stream_supplier_score(&far_peer, &clean, 1, 0) == 136);
    /* Обычная сумма RTT предпочла бы far_peer (85 < 90); удвоенный RTT соседа предпочитает near_peer. */
    CHECK(stream_supplier_score(&near_peer, &clean, 1, 0) < stream_supplier_score(&far_peer, &clean, 1, 0));
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) == 129);
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) < stream_supplier_score(&far_peer, &clean, 1, 0));
    late.late_blocks = 3;
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) > stream_supplier_score(&far_peer, &clean, 1, 0));
    far_peer = near_peer; far_peer.rtt_ms = 9; far_peer.path.count = 4;
    CHECK(stream_supplier_score(&far_peer, &clean, 1, 0) == 110);
    CHECK(stream_supplier_score(&near_peer, &clean, 1, 0) < stream_supplier_score(&far_peer, &clean, 1, 0));
    late = (struct stream_metric_snapshot){.blocks = 4, .late_blocks = 1, .stops = 7};
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) == 369);
    late.lag_ms = STREAM_LATE_MS;
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) == 369);
    late.lag_ms++;
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) == 1109);
    late.lag_ms = 0; late.stopped = 1;
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) == 1109);
    late.stopped = 0; late.late_blocks = late.blocks;
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) == 1109); /* История остановок не превышает лимит штрафа. */
    CHECK(stream_supplier_score(&near_peer, &late, 0, 0) == 109);
    struct stream_receive_info quality = near_peer;
    struct stream_metric_snapshot rate = {.blocks = 100, .source_rate_valid = 1, .receive_rate_valid = 1,
        .source_rate_bytes_sec = 100000, .receive_rate_bytes_sec = 95000};
    CHECK(stream_supplier_score(&quality, &rate, 1, 0) == 109);
    rate.receive_rate_bytes_sec = 90000; CHECK(stream_supplier_score(&quality, &rate, 1, 0) == 209);
    rate.receive_rate_bytes_sec = 50000; CHECK(stream_supplier_score(&quality, &rate, 1, 0) == 609);
    rate.blocks = 19; CHECK(stream_supplier_score(&quality, &rate, 1, 0) == 109);
    rate.blocks = 100; rate.receive_rate_valid = 0; CHECK(stream_supplier_score(&quality, &rate, 1, 0) == 109);
    quality.supplier_queue_age_ms = 300; CHECK(stream_supplier_score(&quality, &clean, 1, 0) == 409);
    quality.live_head_us = 2000000; quality.media_pts_us = 1500000; quality.info_ms = 10000;
    quality.requested_seq = 10; quality.last_available = 9;
    CHECK(stream_supplier_score(&quality, &clean, 1, 15000) == 614); /* max(очередь300, lag500), возраст5. */
    quality.last_available = 10; CHECK(stream_supplier_score(&quality, &clean, 1, 15000) == 414); /* Свежая часть partial relay. */
    quality.media_pts_us = UINT64_MAX; CHECK(stream_supplier_score(&quality, &clean, 1, 15000) == 414);
    quality.supplier_queue_age_ms = UINT64_MAX; CHECK(stream_supplier_score(&quality, &clean, 1, UINT64_MAX) == 1169);
    near_peer.rtt_valid = 0;
    CHECK(stream_supplier_score(&near_peer, &clean, 1, 0) == UINT64_MAX);
    near_peer.rtt_valid = 1; near_peer.path_rtt_valid = 0;
    CHECK(stream_supplier_score(&near_peer, &clean, 1, 0) == UINT64_MAX);
    near_peer.path_rtt_valid = 1; near_peer.rtt_ms = near_peer.path_rtt_ms = UINT32_MAX;
    CHECK(stream_supplier_score(&near_peer, &late, 1, 0) == 3 * (uint64_t)UINT32_MAX + 1009);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] supplier score: RTT, history/rate/content/queue maximum, fresh INFO, unknown and bounded fields");
}

static void test_probe_schedule(void) {
    struct stream_probe_schedule schedule = {0};
    CHECK(stream_probe_schedule_tick(&schedule, 0, 40, 0) == 32 && schedule.cycle == 1);
    CHECK(stream_probe_schedule_tick(&schedule, 999, 8, 32) == 0);
    CHECK(stream_probe_schedule_tick(&schedule, 1000, 8, 32) == 1 && schedule.cycle == 1);
    CHECK(stream_probe_schedule_tick(&schedule, 1001, 7, 32) == 0);
    for (uint64_t now = 2000; now <= 29000; now += 1000)
        CHECK(stream_probe_schedule_tick(&schedule, now, 0, 0) == 1 && schedule.cycle == 1);
    CHECK(stream_probe_schedule_tick(&schedule, 29999, 1, 0) == 0); /* Новая запись не добавляет второй burst. */
    CHECK(stream_probe_schedule_tick(&schedule, 30000, 1, 0) == 1 && schedule.cycle == 1);
    CHECK(stream_probe_schedule_tick(&schedule, 31000, 0, 1) == 1 && schedule.cycle == 1); /* Ещё идёт старая проба. */
    CHECK(stream_probe_schedule_tick(&schedule, 32000, 0, 0) == 1 && schedule.cycle == 2 && schedule.started_ms == 32000);
    CHECK(stream_probe_schedule_tick(&schedule, 32001, 40, 0) == 0);
    memset(&schedule, 0, sizeof(schedule));
    CHECK(stream_probe_schedule_tick(&schedule, 0, 0, 0) == 32);
    CHECK(stream_probe_schedule_tick(&schedule, 30000, 0, 0) == 1 && schedule.cycle == 2);
    CHECK(stream_probe_schedule_tick(&schedule, 60000, 0, 0) == 1 && schedule.cycle == 3);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] probe schedule: one 32-node burst, 1/s, new peers, 30s minimum and unfinished cycles");
}

/* Длительности зависят от кадров/PTS, не от размера RS; продолжения большого кадра считаются один раз. */
static void test_buffer_telemetry(void) {
    struct stream_encoder *encoder = NULL; struct stream_buffer *buffer = NULL;
    CHECK(stream_encoder_create(&encoder, &descriptor, private_key) == STREAM_OK);
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 64 * 1300) == STREAM_OK);
    struct stream_frame_timing timings[] = {
        {.valid = 1, .video = 1, .started = 1, .first_frame = 1, .last_frame = 1,
         .first_pts = 1000000, .last_pts = 1000000, .period_us = 33333, .audio_us = 20000},
        {.valid = 1, .video = 1, .continuation = 1, .first_frame = 1, .last_frame = 1,
         .first_pts = 1000000, .last_pts = 1000000, .period_us = 33333, .audio_us = 20000},
        {.valid = 1, .video = 1, .started = 1, .first_frame = 2, .last_frame = 2,
         .first_pts = 1040000, .last_pts = 1040000, .period_us = 40000, .audio_us = 20000},
        {.valid = 1, .video = 1, .started = 1, .first_frame = 3, .last_frame = 3,
         .first_pts = 1090000, .last_pts = 1090000, .period_us = 50000, .audio_us = 20000}
    };
    struct stream_buffer_delay delay;
    for (unsigned i = 0; i < 4; i++) {
        put_batch(encoder, buffer, i, 255);
        if (!i) CHECK(stream_buffer_get_delay(buffer, 0, 0, &delay) == STREAM_OK && !delay.known);
        uint8_t wire[STREAM_FRAME_TIMING_SIZE]; struct stream_frame_timing timing;
        stream_frame_timing_write(wire, &timings[i]);
        CHECK(stream_frame_timing_read(wire, sizeof(wire), &timing) == STREAM_OK);
        CHECK(stream_buffer_set_timing(buffer, i, &timing) == STREAM_OK);
        wire[45] = 1; CHECK(stream_frame_timing_read(wire, sizeof(wire), &timing) == STREAM_ERR_FORMAT);
    }
    CHECK(stream_buffer_get_delay(buffer, 0, 0, &delay) == STREAM_OK && delay.known && delay.frames == 3);
    CHECK(delay.video_us == 140000 && delay.audio_us == 80000);
    CHECK(stream_buffer_get_delay(buffer, 1, 0, &delay) == STREAM_OK && delay.frames == 3 && delay.video_us == 140000);
    CHECK(stream_buffer_get_delay(buffer, 2, 0, &delay) == STREAM_OK && delay.frames == 2 && delay.video_us == 100000);
    CHECK(stream_buffer_get_delay(buffer, 4, 0, &delay) == STREAM_OK && delay.known && !delay.frames && !delay.video_us);
    struct stream_telemetry t = {.count = 2, .received_ms = 100,
        .rows = {{.node = STREAM_TEST_ADDR(1), .buffered_us = 600000, .frames = 18, .sample = 4, .flags = STREAM_TELEMETRY_KNOWN},
                 {.node = STREAM_TEST_ADDR(2), .buffered_us = 500000, .frames = 15, .sample = 8, .flags = STREAM_TELEMETRY_KNOWN}}};
    uint8_t wire[1 + STREAM_PATH_MAX * STREAM_TELEMETRY_ROW_SIZE];
    size_t length = stream_telemetry_write(wire, &t); struct stream_telemetry parsed;
    CHECK(stream_telemetry_read(wire, length, &parsed) == STREAM_OK && stream_telemetry_total(&parsed) == 1100000);
    CHECK(stream_telemetry_read(wire, length - 1, &parsed) == STREAM_ERR_FORMAT);
    parsed.received_ms = 100; stream_telemetry_age(&parsed, 499);
    CHECK(parsed.rows[0].age_ms == 399 && !(parsed.rows[0].flags & STREAM_TELEMETRY_STALE));
    CHECK(stream_buffer_set_telemetry(buffer, 0, &parsed) == STREAM_OK);
    CHECK(stream_buffer_get_telemetry(buffer, 0, 500, &parsed) == STREAM_OK);
    CHECK(parsed.rows[0].age_ms == 400 && (parsed.rows[0].flags & STREAM_TELEMETRY_STALE));
    CHECK(stream_telemetry_total(&parsed) == 1100000);
    stream_buffer_telemetry_down(buffer, 0, STREAM_TEST_ADDR(2));
    CHECK(stream_buffer_get_telemetry(buffer, 0, 600, &parsed) == STREAM_OK && (parsed.rows[1].flags & STREAM_TELEMETRY_DOWN));
    CHECK(parsed.rows[1].buffered_us == 500000 && !(parsed.rows[0].flags & STREAM_TELEMETRY_DOWN));
    t.rows[1].node = STREAM_TEST_ADDR(1); length = stream_telemetry_write(wire, &t);
    CHECK(stream_telemetry_read(wire, length, &parsed) == STREAM_ERR_FORMAT);
    stream_buffer_destroy(buffer); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] frame buffers: FEC/continuation dedup, variable PTS, cursor, stale 400ms and retained DOWN values");
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_DISABLED);
    CHECK(get_category_by_name("stream") == DEBUG_CATEGORY_STREAM);
    for (unsigned i = 0; i < sizeof(private_key); i++) private_key[i] = (uint8_t)(i * 37 + 11);
    descriptor.source = stream_test_public(private_key);
    size_t allocations = u_get_allocated_count();
    test_signed_blocks(); CHECK(u_get_allocated_count() == allocations);
    test_wire(); CHECK(u_get_allocated_count() == allocations);
    test_load(); CHECK(u_get_allocated_count() == allocations);
    test_probe_schedule(); CHECK(u_get_allocated_count() == allocations);
    test_supplier_score(); CHECK(u_get_allocated_count() == allocations);
    test_fec_and_padding(); CHECK(u_get_allocated_count() == allocations);
    test_variable_batches(); CHECK(u_get_allocated_count() == allocations);
    test_rejected_batches(); CHECK(u_get_allocated_count() == allocations);
    test_ring_and_conflicts(); CHECK(u_get_allocated_count() == allocations);
    test_buffer_telemetry(); CHECK(u_get_allocated_count() == allocations);
    test_relative_metrics(); CHECK(u_get_allocated_count() == allocations);
    test_source_timer(); CHECK(u_get_allocated_count() == allocations);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] stream core: all checks, no tracked allocations remain");
    return 0;
}
