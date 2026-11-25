#include "manifest.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/strbuf.h"
#include <limits.h>
#include <string.h>
#include <openssl/evp.h>

char* attachment_manifest_format(const void* description, size_t length, const struct media_index_result* result,
    uint32_t duration_ms, uint16_t width, uint16_t height) {
    if (!description || !length || length > ATTACHMENT_MANIFEST_BODY_MAX || !result || result->file_size <= 0 ||
        result->block_size <= 0 || result->num_blocks < 1 || result->num_blocks > 100 || !result->block_ids || !result->block_sigs ||
        (result->file_size - 1) / result->block_size + 1 != result->num_blocks || (duration_ms && (!width || !height))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment manifest: invalid description/geometry/dimensions"); return NULL;
    }
    size_t capacity = ((length + 2) / 3) * 4 + 1; unsigned char* encoded = u_malloc((uint32_t)capacity);
    if (!encoded) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment manifest: description encoding allocation failed bytes=%zu", length); return NULL; }
    int written = EVP_EncodeBlock(encoded, description, (int)length);
    if (written < 0 || (size_t)written >= capacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment manifest: description encoding failed bytes=%zu", length); u_free(encoded); return NULL;
    }
    for (int i = 0; i < written; i++) { if (encoded[i] == '+') encoded[i] = '-'; else if (encoded[i] == '/') encoded[i] = '_'; }
    struct strbuf output = {0};
    int failed = strbuf_addf(&output, "*%s|%lld|%lld|%d|", encoded, (long long)result->file_size,
        (long long)result->block_size, result->num_blocks) < 0;
    u_free(encoded);
    for (unsigned i = 0; !failed && i < 16; i++) failed = strbuf_addf(&output, "%02x", result->media_id[i]) < 0;
    if (strbuf_addf(&output, "|") < 0) failed = 1;
    for (unsigned i = 0; !failed && i < 32; i++) failed = strbuf_addf(&output, "%02x", result->content_hash[i]) < 0;
    if (strbuf_addf(&output, "|") < 0) failed = 1;
    for (int block = 0; !failed && block < result->num_blocks; block++) {
        if (block && strbuf_addf(&output, ",") < 0) failed = 1;
        for (unsigned i = 0; !failed && i < 16; i++) failed = strbuf_addf(&output, "%02x", result->block_ids[block * 16 + i]) < 0;
        if (strbuf_addf(&output, ",") < 0) failed = 1;
        for (unsigned i = 0; !failed && i < 64; i++) failed = strbuf_addf(&output, "%02x", result->block_sigs[block * 64 + i]) < 0;
    }
    if (width || height) {
        if (duration_ms && strbuf_addf(&output, "|dur=%u", duration_ms) < 0) failed = 1;
        if (strbuf_addf(&output, "|w=%u|h=%u", width, height) < 0) failed = 1;
    }
    if (output.len > ATTACHMENT_MANIFEST_BODY_MAX) failed = 1;
    if (failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment manifest: formatting failed/limit exceeded bytes=%zu blocks=%d", output.len, result->num_blocks);
        strbuf_free(&output); return NULL;
    }
    return strbuf_detach(&output);
}

/* Беззнаковое десятичное число до delimiter; delimiter=0 требует точного конца строки. */
static int msg_media_number(const char** cursor, const char* end, uint64_t* value, char delimiter) {
    const char* p = *cursor; uint64_t number = 0;
    if (p == end || *p < '0' || *p > '9') return -1;
    while (p < end && *p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (number > ((uint64_t)INT64_MAX - digit) / 10) return -1;
        number = number * 10 + digit;
    }
    if (delimiter ? (p == end || *p++ != delimiter) : p != end) return -1;
    *cursor = p; *value = number; return 0;
}

static int msg_media_hex(const char** cursor, const char* end, uint8_t* out, size_t bytes, char delimiter) {
    const char* p = *cursor;
    if ((size_t)(end - p) < bytes * 2 + (delimiter ? 1 : 0)) return -1;
    for (size_t i = 0; i < bytes; i++) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; j++) {
            unsigned c = (unsigned char)*p++, digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return -1;
            value = value * 16 + digit;
        }
        out[i] = (uint8_t)value;
    }
    if (delimiter && *p++ != delimiter) return -1;
    *cursor = p; return 0;
}

/* Проверяет блоки и необязательный хвост |[dur=<ms>|]w=<width>|h=<height>; памяти хвоста не выделяет. */
int attachment_manifest_parse(const char* body, struct media_index_result* output) {
    if (!body || !output) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media manifest: body/result required"); return -1; }
    memset(output, 0, sizeof(*output));
    const char* stage = "length/name";
    size_t length = strnlen(body, ATTACHMENT_MANIFEST_BODY_MAX + 1);
    if (length > ATTACHMENT_MANIFEST_BODY_MAX) goto invalid;
    const char* end = body + length; const char* p = memchr(body, '|', length);
    if (!p) return -1; /* Обычный текст также проходит через scans. */
    if (p == body) goto invalid;
    p++;
    uint64_t size, block, count;
    stage = "block geometry";
    if (msg_media_number(&p, end, &size, '|') || msg_media_number(&p, end, &block, '|') ||
        msg_media_number(&p, end, &count, '|') || !size || !block || !count ||
        count > 100 || (size - 1) / block + 1 != count) goto invalid;
    struct media_index_result result = {.file_size = (int64_t)size, .block_size = (int64_t)block, .num_blocks = (int)count};
    stage = "media identity";
    if (msg_media_hex(&p, end, result.media_id, 16, '|') || msg_media_hex(&p, end, result.content_hash, 32, '|')) goto invalid;
    const char* pairs_end = memchr(p, '|', (size_t)(end - p));
    if (!pairs_end) pairs_end = end;
    stage = "block signature length";
    if ((size_t)(pairs_end - p) != (size_t)count * 162 - 1) goto invalid;
    if (pairs_end < end) {
        const char* metadata = pairs_end + 1; uint64_t duration, width, height;
        stage = "media dimensions/duration";
        if (end - metadata >= 4 && !memcmp(metadata, "dur=", 4)) {
            metadata += 4;
            if (msg_media_number(&metadata, end, &duration, '|') || !duration) goto invalid;
        }
        if (end - metadata < 2 || memcmp(metadata, "w=", 2)) goto invalid;
        metadata += 2;
        if (msg_media_number(&metadata, end, &width, '|') || width > INT_MAX || end - metadata < 2 ||
            memcmp(metadata, "h=", 2)) goto invalid;
        metadata += 2;
        if (msg_media_number(&metadata, end, &height, 0) || height > INT_MAX || (!width && !height)) goto invalid;
    }
    result.block_ids = u_malloc((size_t)count * 16); result.block_sigs = u_malloc((size_t)count * 64);
    if (!result.block_ids || !result.block_sigs) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media manifest: allocation failed blocks=%llu", (unsigned long long)count);
        media_index_result_free(&result); return -2;
    }
    stage = "block signature encoding";
    for (unsigned i = 0; i < count; i++) {
        if (msg_media_hex(&p, pairs_end, result.block_ids + i * 16, 16, ',') ||
            msg_media_hex(&p, pairs_end, result.block_sigs + i * 64, 64, i + 1 < count ? ',' : 0)) {
            media_index_result_free(&result); goto invalid;
        }
    }
    *output = result; return 0;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media manifest: invalid %s bytes=%zu", stage, length); return -1;
}

