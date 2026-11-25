#include <string.h>

#include "mem.h"
#include "debug_config.h"
#include "reed_solomon.h"

struct rs_codec {
    unsigned n;
    unsigned k;
    size_t max_block_size;
    uint8_t gf_log[256];
    uint8_t gf_exp[510];
    uint8_t *generator;
    uint8_t *work;
    uint8_t storage[];
};

/* Умножение в GF(256) с примитивным полиномом x^8+x^4+x^3+x^2+1 (0x11d). */
static uint8_t rs_multiply(const struct rs_codec *ctx, uint8_t a, uint8_t b) {
    return a && b ? ctx->gf_exp[ctx->gf_log[a] + ctx->gf_log[b]] : 0;
}

/* Обратить k строк матрицы; результат — правая половина рабочей матрицы [A|I]. */
static int rs_invert(struct rs_codec *ctx, const uint8_t *const rows[]) {
    unsigned k = ctx->k;
    unsigned stride = 2 * k;
    for (unsigned i = 0; i < k; i++) {
        uint8_t *row = ctx->work + i * stride;
        memcpy(row, rows[i], k);
        memset(row + k, 0, k);
        row[k + i] = 1;
    }
    for (unsigned col = 0; col < k; col++) {
        unsigned pivot = col;
        while (pivot < k && !ctx->work[pivot * stride + col]) pivot++;
        if (pivot == k) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: singular matrix n=%u k=%u column=%u", ctx->n, k, col);
            return RS_ERR_MATRIX;
        }
        uint8_t *row = ctx->work + col * stride;
        if (pivot != col) {
            uint8_t *other = ctx->work + pivot * stride;
            for (unsigned j = 0; j < stride; j++) {
                uint8_t tmp = row[j];
                row[j] = other[j];
                other[j] = tmp;
            }
        }
        uint8_t inverse = ctx->gf_exp[255 - ctx->gf_log[row[col]]];
        for (unsigned j = col; j < stride; j++) row[j] = rs_multiply(ctx, row[j], inverse);
        for (unsigned i = 0; i < k; i++) {
            if (i == col) continue;
            uint8_t *other = ctx->work + i * stride;
            uint8_t factor = other[col];
            if (!factor) continue;
            for (unsigned j = col; j < stride; j++) other[j] ^= rs_multiply(ctx, factor, row[j]);
        }
    }
    return RS_OK;
}

/* Построить систематическую матрицу G=V*inverse(V[0:k]) в точках 2^i поля GF(256). */
static int rs_build_generator(struct rs_codec *ctx) {
    unsigned k = ctx->k;
    if (ctx->n != k) {
        const uint8_t *rows[RS_MAX_BLOCKS];
        for (unsigned i = 0; i < ctx->n; i++) {
            uint8_t value = 1;
            for (unsigned j = 0; j < k; j++) {
                ctx->generator[i * k + j] = value;
                value = rs_multiply(ctx, value, ctx->gf_exp[i]);
            }
        }
        for (unsigned i = 0; i < k; i++) rows[i] = ctx->generator + i * k;
        int rc = rs_invert(ctx, rows);
        if (rc != RS_OK) return rc;
        for (unsigned i = k; i < ctx->n; i++) {
            uint8_t result[RS_MAX_BLOCKS];
            for (unsigned j = 0; j < k; j++) {
                uint8_t value = 0;
                for (unsigned t = 0; t < k; t++) {
                    value ^= rs_multiply(ctx, ctx->generator[i * k + t], ctx->work[t * 2 * k + k + j]);
                }
                result[j] = value;
            }
            memcpy(ctx->generator + i * k, result, k);
        }
    }
    memset(ctx->generator, 0, k * k);
    for (unsigned i = 0; i < k; i++) ctx->generator[i * k + i] = 1;
    return RS_OK;
}

/* Инициализация параметров, таблиц поля и матрицы; все рабочие данные в одной аллокации. */
int rs_init(struct rs_codec **out, unsigned n, unsigned r, size_t block_size) {
    if (out) *out = NULL;
    if (!out || !n || n > RS_MAX_BLOCKS || r >= n || block_size <= RS_HEADER_SIZE || block_size > RS_MAX_BLOCK_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: invalid init ctx=%p n=%u r=%u block_size=%zu", (void *)out, n, r, block_size);
        return RS_ERR_INVALID_ARG;
    }
    unsigned k = n - r;
    size_t bytes = sizeof(struct rs_codec) + n * k + 2 * k * k;
    struct rs_codec *ctx = u_calloc(1, (uint32_t)bytes);
    if (!ctx) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: allocation failed n=%u r=%u block_size=%zu bytes=%zu", n, r, block_size, bytes);
        return RS_ERR_MEMORY;
    }
    ctx->n = n;
    ctx->k = k;
    ctx->max_block_size = block_size;
    ctx->generator = ctx->storage;
    ctx->work = ctx->storage + n * k;
    unsigned value = 1;
    for (unsigned i = 0; i < 255; i++) {
        ctx->gf_exp[i] = (uint8_t)value;
        ctx->gf_log[value] = (uint8_t)i;
        value <<= 1;
        if (value & 256) value ^= 0x11d;
    }
    for (unsigned i = 255; i < 510; i++) ctx->gf_exp[i] = ctx->gf_exp[i - 255];
    int rc = rs_build_generator(ctx);
    if (rc != RS_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: generator failed n=%u k=%u rc=%d", n, k, rc);
        u_free(ctx);
        return rc;
    }
    *out = ctx;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "reed_solomon: initialized n=%u r=%u max_block_size=%zu input_capacity=%zu memory=%zu",
               n, r, block_size, k * (block_size - RS_HEADER_SIZE), bytes);
    return RS_OK;
}

void rs_free(struct rs_codec *ctx) {
    if (!ctx) return;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "reed_solomon: free n=%u r=%u max_block_size=%zu", ctx->n, ctx->n - ctx->k, ctx->max_block_size);
    u_free(ctx);
}

size_t rs_input_capacity(const struct rs_codec *ctx) {
    if (!ctx) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: input_capacity requires a context");
        return 0;
    }
    return ctx->k * (ctx->max_block_size - RS_HEADER_SIZE);
}

/* Записать заголовки и исходные субблоки, затем вычислить r проверочных субблоков. */
int rs_encode(struct rs_codec *ctx, uint64_t seq, const uint8_t *input, size_t length,
              uint8_t *const blocks[], size_t *block_size) {
    if (!ctx || !input || !blocks || !block_size || !length || length > rs_input_capacity(ctx)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: invalid encode ctx=%p input=%p blocks=%p length=%zu size_out=%p",
                    (void *)ctx, (const void *)input, (const void *)blocks, length, (void *)block_size);
        return RS_ERR_INVALID_ARG;
    }
    size_t payload_size = (length + ctx->k - 1) / ctx->k;
    for (unsigned i = 0; i < ctx->n; i++) {
        if (!blocks[i]) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: missing encode buffer seq=%llu subblock=%u", (unsigned long long)seq, i);
            return RS_ERR_INVALID_ARG;
        }
    }
    for (unsigned i = 0; i < ctx->n; i++) {
        for (unsigned j = 0; j < 8; j++) blocks[i][j] = (uint8_t)(seq >> (56 - 8 * j));
        blocks[i][8] = (uint8_t)i;
        uint8_t *payload = blocks[i] + RS_HEADER_SIZE;
        memset(payload, 0, payload_size);
        if (i < ctx->k) {
            size_t offset = i * payload_size;
            size_t take = offset < length ? length - offset : 0;
            if (take > payload_size) take = payload_size;
            if (take) memcpy(payload, input + offset, take);
            continue;
        }
        for (unsigned j = 0; j < ctx->k; j++) {
            uint8_t factor = ctx->generator[i * ctx->k + j];
            if (!factor) continue;
            const uint8_t *source = blocks[j] + RS_HEADER_SIZE;
            for (size_t b = 0; b < payload_size; b++) payload[b] ^= rs_multiply(ctx, factor, source[b]);
        }
    }
    *block_size = RS_HEADER_SIZE + payload_size;
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "reed_solomon: encoded seq=%llu n=%u r=%u length=%zu padding=%zu block_size=%zu max=%zu",
                (unsigned long long)seq, ctx->n, ctx->n - ctx->k, length, ctx->k * payload_size - length,
                *block_size, ctx->max_block_size);
    return RS_OK;
}

/* Проверить всю пачку и восстановить только отсутствующие исходные субблоки. */
int rs_decode(struct rs_codec *ctx, const uint8_t *const blocks[], size_t block_size,
              uint8_t *output, size_t output_capacity) {
    if (!ctx || !blocks || !output || block_size <= RS_HEADER_SIZE || block_size > ctx->max_block_size
        || output_capacity < ctx->k * (block_size - RS_HEADER_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: invalid decode ctx=%p blocks=%p output=%p block_size=%zu capacity=%zu",
                    (void *)ctx, (const void *)blocks, (void *)output, block_size, output_capacity);
        return RS_ERR_INVALID_ARG;
    }
    size_t payload_size = block_size - RS_HEADER_SIZE;
    const uint8_t *rows[RS_MAX_BLOCKS];
    unsigned selected[RS_MAX_BLOCKS];
    unsigned count = 0;
    unsigned missing = 0;
    uint64_t seq = 0;
    for (unsigned i = 0; i < ctx->n; i++) {
        if (!blocks[i]) {
            if (i < ctx->k) missing++;
            continue;
        }
        uint64_t block_seq = 0;
        for (unsigned j = 0; j < 8; j++) block_seq = (block_seq << 8) | blocks[i][j];
        if (blocks[i][8] != i || (count && block_seq != seq)) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: invalid header slot=%u subblock=%u seq=%llu expected_seq=%llu",
                        i, blocks[i][8], (unsigned long long)block_seq, (unsigned long long)(count ? seq : block_seq));
            return RS_ERR_HEADER;
        }
        seq = block_seq;
        if (count < ctx->k) {
            selected[count] = i;
            rows[count] = ctx->generator + i * ctx->k;
        }
        count++;
    }
    if (count < ctx->k) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "reed_solomon: insufficient blocks seq=%llu received=%u required=%u n=%u",
                    (unsigned long long)seq, count, ctx->k, ctx->n);
        return RS_ERR_INSUFFICIENT;
    }
    if (missing) {
        int rc = rs_invert(ctx, rows);
        if (rc != RS_OK) return rc;
    }
    for (unsigned i = 0; i < ctx->k; i++) {
        uint8_t *target = output + i * payload_size;
        if (blocks[i]) {
            memcpy(target, blocks[i] + RS_HEADER_SIZE, payload_size);
            continue;
        }
        memset(target, 0, payload_size);
        for (unsigned j = 0; j < ctx->k; j++) {
            uint8_t factor = ctx->work[i * 2 * ctx->k + ctx->k + j];
            if (!factor) continue;
            const uint8_t *source = blocks[selected[j]] + RS_HEADER_SIZE;
            for (size_t b = 0; b < payload_size; b++) target[b] ^= rs_multiply(ctx, factor, source[b]);
        }
        DEBUG_TRACE(DEBUG_CATEGORY_GENERAL, "reed_solomon: recovered seq=%llu subblock=%u", (unsigned long long)seq, i);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "reed_solomon: decoded seq=%llu received=%u recovered=%u output_size=%zu",
                (unsigned long long)seq, count, missing, ctx->k * payload_size);
    return RS_OK;
}
