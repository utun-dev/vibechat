// Пачки RS: независимый эталон, пропуски, размеры, заголовки и внешняя сборка потока.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reed_solomon.h"
#include "mem.h"
#include "debug_config.h"

#define CHECK(expr) do { if (!(expr)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "test_reed_solomon: failed line=%d: %s", __LINE__, #expr); exit(1); \
} } while (0)
#define GUARD 8
#define MARKER 0xcd

struct fixture {
    struct rs_codec *ctx;
    unsigned n;
    unsigned k;
    size_t block_size;
    size_t input_size;
    size_t storage_size;
    uint8_t *input_storage;
    uint8_t *output_storage;
    uint8_t *storage;
    uint8_t *saved;
    uint8_t *input;
    uint8_t *output;
    uint8_t *blocks[RS_MAX_BLOCKS];
    const uint8_t *received[RS_MAX_BLOCKS];
};

/* Независимая арифметика поля без таблиц, для эталона через интерполяцию Лагранжа. */
static uint8_t reference_multiply(uint8_t a, uint8_t b) {
    unsigned value = a;
    unsigned result = 0;
    while (b) {
        if (b & 1) result ^= value;
        b >>= 1;
        value <<= 1;
        if (value & 256) value ^= 0x11d;
    }
    return (uint8_t)result;
}

static uint8_t reference_power(uint8_t a, unsigned power) {
    uint8_t result = 1;
    while (power--) result = reference_multiply(result, a);
    return result;
}

/* Проверить защитные байты вокруг всех предоставленных кодеку буферов. */
static void check_guards(const struct fixture *f) {
    for (unsigned i = 0; i < GUARD; i++) {
        CHECK(f->input_storage[i] == MARKER && f->input[f->input_size + i] == MARKER);
        CHECK(f->output_storage[i] == MARKER && f->output[f->input_size + i] == MARKER);
        for (unsigned j = 0; j < f->n; j++) {
            CHECK(f->blocks[j][(int)i - GUARD] == MARKER);
            CHECK(f->blocks[j][f->block_size + i] == MARKER);
        }
    }
}

/* Выделить буферы с защитными байтами и закодировать детерминированные исходные данные. */
static void fixture_init(struct fixture *f, unsigned n, unsigned r, size_t block_size, uint64_t seq) {
    memset(f, 0, sizeof(*f));
    f->n = n;
    f->k = n - r;
    f->block_size = block_size;
    CHECK(rs_init(&f->ctx, n, r, block_size) == RS_OK);
    f->input_size = rs_input_capacity(f->ctx);
    CHECK(f->input_size == f->k * (block_size - RS_HEADER_SIZE));
    f->storage_size = n * (block_size + 2 * GUARD);
    f->input_storage = u_malloc((uint32_t)(f->input_size + 2 * GUARD));
    f->output_storage = u_malloc((uint32_t)(f->input_size + 2 * GUARD));
    f->storage = u_malloc((uint32_t)f->storage_size);
    f->saved = u_malloc((uint32_t)f->storage_size);
    CHECK(f->input_storage && f->output_storage && f->storage && f->saved);
    memset(f->input_storage, MARKER, f->input_size + 2 * GUARD);
    memset(f->output_storage, MARKER, f->input_size + 2 * GUARD);
    memset(f->storage, MARKER, f->storage_size);
    f->input = f->input_storage + GUARD;
    f->output = f->output_storage + GUARD;
    uint32_t random = 0x5eed1234;
    for (size_t i = 0; i < f->input_size; i++) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        f->input[i] = (uint8_t)random;
    }
    for (unsigned i = 0; i < n; i++) {
        f->blocks[i] = f->storage + i * (block_size + 2 * GUARD) + GUARD;
        f->received[i] = f->blocks[i];
    }
    size_t allocations = u_get_allocated_count();
    size_t encoded_size = 0;
    CHECK(rs_encode(f->ctx, seq, f->input, f->input_size, f->blocks, &encoded_size) == RS_OK && encoded_size == block_size);
    CHECK(u_get_allocated_count() == allocations);
    memcpy(f->saved, f->storage, f->storage_size);
    for (unsigned i = 0; i < n; i++) {
        for (unsigned j = 0; j < 8; j++) CHECK(f->blocks[i][j] == (uint8_t)(seq >> (56 - 8 * j)));
        CHECK(f->blocks[i][8] == i);
        if (i < f->k) CHECK(memcmp(f->blocks[i] + RS_HEADER_SIZE, f->input + i * (block_size - RS_HEADER_SIZE),
                                 block_size - RS_HEADER_SIZE) == 0);
    }
    check_guards(f);
}

static void fixture_free(struct fixture *f) {
    check_guards(f);
    u_free(f->input_storage);
    u_free(f->output_storage);
    u_free(f->storage);
    u_free(f->saved);
    rs_free(f->ctx);
}

/* Кодограммы decode остаются неизменными, выход совпадает побайтово, аллокаций нет. */
static void check_decode(struct fixture *f) {
    size_t allocations = u_get_allocated_count();
    memset(f->output, MARKER, f->input_size);
    CHECK(rs_decode(f->ctx, f->received, f->block_size, f->output, f->input_size) == RS_OK);
    CHECK(u_get_allocated_count() == allocations);
    CHECK(memcmp(f->input, f->output, f->input_size) == 0);
    CHECK(memcmp(f->storage, f->saved, f->storage_size) == 0);
    check_guards(f);
}

/* Сравнить проверочные субблоки с независимой интерполяцией по исходным значениям. */
static void check_reference(const struct fixture *f) {
    size_t payload_size = f->block_size - RS_HEADER_SIZE;
    uint8_t coefficients[RS_MAX_BLOCKS];
    for (unsigned i = f->k; i < f->n; i++) {
        uint8_t point = reference_power(2, i);
        for (unsigned j = 0; j < f->k; j++) {
            uint8_t source_point = reference_power(2, j);
            uint8_t coefficient = 1;
            for (unsigned t = 0; t < f->k; t++) {
                if (t == j) continue;
                uint8_t other = reference_power(2, t);
                uint8_t inverse = reference_power(source_point ^ other, 254);
                coefficient = reference_multiply(coefficient, reference_multiply(point ^ other, inverse));
            }
            coefficients[j] = coefficient;
        }
        for (size_t b = 0; b < payload_size; b++) {
            uint8_t expected = 0;
            for (unsigned j = 0; j < f->k; j++) expected ^= reference_multiply(coefficients[j], f->input[j * payload_size + b]);
            CHECK(f->blocks[i][RS_HEADER_SIZE + b] == expected);
        }
    }
}

/* Для 16/2 перебрать отсутствие потерь, все одиночные потери и все 120 пар. */
static void test_all_pairs(void) {
    const size_t sizes[] = {10, 11, 16, 255, 1000, 4095, 4096};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        struct fixture f;
        fixture_init(&f, 16, 2, sizes[s], UINT64_C(0x0123456789abcdef));
        check_reference(&f);
        check_decode(&f);
        for (unsigned i = 0; i < 16; i++) {
            f.received[i] = NULL;
            check_decode(&f);
            for (unsigned j = i + 1; j < 16; j++) {
                f.received[j] = NULL;
                check_decode(&f);
                f.received[j] = f.blocks[j];
            }
            f.received[i] = f.blocks[i];
        }
        fixture_free(&f);
    }
    puts("[PASS] reed_solomon: 16/2 all single losses and 120 pairs, seven block sizes, independent parity reference");
}

/* Каждый полный размер кодограммы от 10 до 4096, seq=0 и seq=UINT64_MAX. */
static void test_all_sizes(void) {
    for (size_t size = RS_HEADER_SIZE + 1; size <= RS_MAX_BLOCK_SIZE; size++) {
        struct fixture f;
        fixture_init(&f, 3, 1, size, size & 1 ? UINT64_MAX : 0);
        f.received[size % 2] = NULL;
        check_decode(&f);
        fixture_free(&f);
    }
    puts("[PASS] reed_solomon: all block sizes 10..4096, seq zero and maximum");
}

/* Независимый фиксированный вектор RS(4,2): P0=2*D0+3*D1, P1=6*D0+7*D1. */
static void test_known_vector(void) {
    struct fixture f;
    fixture_init(&f, 4, 2, 10, 7);
    f.input[0] = 1;
    f.input[1] = 2;
    size_t encoded_size = 0;
    CHECK(rs_encode(f.ctx, 7, f.input, f.input_size, f.blocks, &encoded_size) == RS_OK && encoded_size == f.block_size);
    CHECK(f.blocks[2][9] == 4 && f.blocks[3][9] == 8);
    memcpy(f.saved, f.storage, f.storage_size);
    f.received[0] = NULL;
    f.received[1] = NULL;
    check_decode(&f);
    fixture_free(&f);
    puts("[PASS] reed_solomon: fixed parity vector and recovery from parity alone");
}

/* Перебрать все достаточные наборы для небольших схем с разным числом резервных потоков. */
static void test_configurations(void) {
    const unsigned configs[][2] = {{1, 0}, {4, 0}, {4, 1}, {6, 3}, {8, 6}, {8, 7}};
    for (unsigned c = 0; c < sizeof(configs) / sizeof(configs[0]); c++) {
        struct fixture f;
        fixture_init(&f, configs[c][0], configs[c][1], 31, c);
        check_reference(&f);
        for (unsigned mask = 0; mask < (1u << f.n); mask++) {
            unsigned count = 0;
            for (unsigned i = 0; i < f.n; i++) {
                f.received[i] = mask & (1u << i) ? f.blocks[i] : NULL;
                if (f.received[i]) count++;
            }
            if (count >= f.k) check_decode(&f);
        }
        fixture_free(&f);
    }
    const unsigned limits[][2] = {{255, 0}, {255, 1}, {255, 253}, {255, 254}};
    for (unsigned c = 0; c < sizeof(limits) / sizeof(limits[0]); c++) {
        struct fixture f;
        fixture_init(&f, limits[c][0], limits[c][1], 10, UINT64_MAX);
        for (unsigned i = 0; i < limits[c][1]; i++) f.received[i] = NULL;
        check_decode(&f);
        fixture_free(&f);
    }
    puts("[PASS] reed_solomon: configurable redundancy, exhaustive small sets and 255-block limits");
}

static void check_unchanged_output(const struct fixture *f) {
    for (size_t i = 0; i < f->input_size; i++) CHECK(f->output[i] == MARKER);
    check_guards(f);
}

/* Невалидные аргументы и заголовки не меняют выход, даже при наличии всех исходных субблоков. */
static void test_errors(void) {
    struct rs_codec *ctx = NULL;
    CHECK(rs_init(NULL, 16, 2, 4096) == RS_ERR_INVALID_ARG);
    const unsigned configs[][2] = {{0, 0}, {256, 1}, {16, 16}, {16, 17}};
    for (unsigned i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        CHECK(rs_init(&ctx, configs[i][0], configs[i][1], 10) == RS_ERR_INVALID_ARG && !ctx);
    }
    const size_t sizes[] = {0, 9, 4097, SIZE_MAX};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        CHECK(rs_init(&ctx, 16, 2, sizes[i]) == RS_ERR_INVALID_ARG && !ctx);
    }
    CHECK(rs_input_capacity(NULL) == 0);
    rs_free(NULL);
    struct fixture f;
    fixture_init(&f, 16, 2, 23, 100);
    size_t encoded_size = 123;
    CHECK(rs_encode(NULL, 100, f.input, f.input_size, f.blocks, &encoded_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_encode(f.ctx, 100, NULL, f.input_size, f.blocks, &encoded_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_encode(f.ctx, 100, f.input, f.input_size, NULL, &encoded_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_encode(f.ctx, 100, f.input, 0, f.blocks, &encoded_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_encode(f.ctx, 100, f.input, f.input_size + 1, f.blocks, &encoded_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_encode(f.ctx, 100, f.input, SIZE_MAX, f.blocks, &encoded_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_encode(f.ctx, 100, f.input, f.input_size, f.blocks, NULL) == RS_ERR_INVALID_ARG);
    CHECK(encoded_size == 123);
    f.blocks[15] = NULL;
    CHECK(rs_encode(f.ctx, 101, f.input, f.input_size, f.blocks, &encoded_size) == RS_ERR_INVALID_ARG && encoded_size == 123);
    CHECK(memcmp(f.storage, f.saved, f.storage_size) == 0);
    f.blocks[15] = (uint8_t *)f.received[15];
    CHECK(rs_decode(NULL, f.received, f.block_size, f.output, f.input_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_decode(f.ctx, NULL, f.block_size, f.output, f.input_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_decode(f.ctx, f.received, f.block_size, NULL, f.input_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_decode(f.ctx, f.received, RS_HEADER_SIZE, f.output, f.input_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_decode(f.ctx, f.received, f.block_size + 1, f.output, f.input_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_decode(f.ctx, f.received, SIZE_MAX, f.output, f.input_size) == RS_ERR_INVALID_ARG);
    CHECK(rs_decode(f.ctx, f.received, f.block_size, f.output, f.input_size - 1) == RS_ERR_INVALID_ARG);
    check_unchanged_output(&f);
    f.received[0] = f.received[1] = f.received[2] = NULL;
    CHECK(rs_decode(f.ctx, f.received, f.block_size, f.output, f.input_size) == RS_ERR_INSUFFICIENT);
    check_unchanged_output(&f);
    for (unsigned i = 0; i < 16; i++) f.received[i] = NULL;
    CHECK(rs_decode(f.ctx, f.received, f.block_size, f.output, f.input_size) == RS_ERR_INSUFFICIENT);
    check_unchanged_output(&f);
    for (unsigned i = 0; i < 16; i++) f.received[i] = f.blocks[i];
    f.blocks[15][0] ^= 1;
    CHECK(rs_decode(f.ctx, f.received, f.block_size, f.output, f.input_size) == RS_ERR_HEADER);
    check_unchanged_output(&f);
    f.blocks[15][0] ^= 1;
    f.blocks[15][8] = 0;
    CHECK(rs_decode(f.ctx, f.received, f.block_size, f.output, f.input_size) == RS_ERR_HEADER);
    check_unchanged_output(&f);
    f.blocks[15][8] = 255;
    CHECK(rs_decode(f.ctx, f.received, f.block_size, f.output, f.input_size) == RS_ERR_HEADER);
    f.blocks[15][8] = 15;
    f.received[0] = f.blocks[1];
    f.received[1] = f.blocks[0];
    CHECK(rs_decode(f.ctx, f.received, f.block_size, f.output, f.input_size) == RS_ERR_HEADER);
    check_unchanged_output(&f);
    f.received[0] = f.blocks[0];
    f.received[1] = f.blocks[1];
    check_decode(&f);
    fixture_free(&f);
    puts("[PASS] reed_solomon: invalid parameters, missing buffers, insufficient blocks, mixed seq and invalid indices");
}

/* Один контекст принимает разные длины; вход точно ограничен, выход не затрагивает хвосты буферов. */
static void test_variable_lengths(void) {
    struct fixture f;
    fixture_init(&f, 8, 2, 4096, 0);
    const size_t lengths[] = {1, 5, 6, 7, 17, 97, 1000, 7080, 24521, 24522, 1};
    for (unsigned step = 0; step < sizeof(lengths) / sizeof(lengths[0]); step++) {
        size_t length = lengths[step], size = 0;
        uint8_t *input = u_malloc((uint32_t)length);
        CHECK(input);
        memcpy(input, f.input, length);
        memset(f.storage, MARKER, f.storage_size);
        memset(f.output, MARKER, f.input_size);
        size_t allocations = u_get_allocated_count();
        CHECK(rs_encode(f.ctx, step, input, length, f.blocks, &size) == RS_OK);
        CHECK(size == RS_HEADER_SIZE + (length + f.k - 1) / f.k);
        for (unsigned i = 0; i < f.n; i++) {
            for (size_t b = size; b < f.block_size; b++) CHECK(f.blocks[i][b] == MARKER);
            f.received[i] = i == 0 || i == 3 ? NULL : f.blocks[i];
        }
        size_t padded = f.k * (size - RS_HEADER_SIZE);
        CHECK(rs_decode(f.ctx, f.received, size, f.output, padded) == RS_OK);
        CHECK(!memcmp(input, f.output, length));
        for (size_t i = length; i < padded; i++) CHECK(f.output[i] == 0);
        for (size_t i = padded; i < f.input_size; i++) CHECK(f.output[i] == MARKER);
        CHECK(u_get_allocated_count() == allocations);
        u_free(input);
    }
    fixture_free(&f);
    puts("[PASS] reed_solomon: changing batch lengths, exact input bounds, minimal zero padding, bounded writes, no per-call allocations");
}

/* Приложение разбивает поток, кодек уменьшает хвостовую пачку, сборка сохраняет исходную длину. */
static void test_stream(void) {
    struct fixture f;
    fixture_init(&f, 16, 2, 101, 0);
    size_t length = 3 * f.input_size + 17;
    uint8_t *source = u_malloc((uint32_t)length);
    uint8_t *result = u_malloc((uint32_t)length);
    CHECK(source && result);
    for (size_t i = 0; i < length; i++) source[i] = (uint8_t)(i * 17 + i / 251);
    size_t offset = 0;
    for (uint64_t seq = 0; offset < length; seq++) {
        size_t valid = length - offset;
        if (valid > f.input_size) valid = f.input_size;
        memcpy(f.input, source + offset, valid);
        size_t encoded_size = 0;
        CHECK(rs_encode(f.ctx, seq, f.input, valid, f.blocks, &encoded_size) == RS_OK);
        memcpy(f.saved, f.storage, f.storage_size);
        for (unsigned i = 0; i < f.n; i++) f.received[i] = NULL;
        for (unsigned i = f.n; i > 0; i--) {
            unsigned index = f.blocks[i - 1][8];
            if (index == seq % f.n || index == (seq + 1) % f.n) continue;
            f.received[index] = f.blocks[i - 1];
        }
        CHECK(rs_decode(f.ctx, f.received, encoded_size, f.output, f.input_size) == RS_OK);
        CHECK(!memcmp(f.input, f.output, valid));
        memcpy(result + offset, f.output, valid);
        offset += valid;
    }
    CHECK(memcmp(source, result, length) == 0);
    u_free(source);
    u_free(result);
    fixture_free(&f);
    puts("[PASS] reed_solomon: external stream splitting and exact reassembly with two losses per batch and padded tail");
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_WARN);
    size_t baseline = u_get_allocated_count();
    test_known_vector();
    test_all_pairs();
    test_all_sizes();
    test_configurations();
    test_errors();
    test_variable_lengths();
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_TRACE);
    test_stream();
    CHECK(u_get_allocated_count() == baseline);
    puts("[PASS] reed_solomon: all tests passed, no memory leaks");
    return 0;
}
