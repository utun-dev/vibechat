#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "stream_block.h"

struct stream_encoder {
    struct stream_descriptor descriptor;
    struct rs_codec *rs;
    uint8_t private_key[SC_PRIVKEY_SIZE];
    uint8_t public_key[SC_PUBKEY_SIZE];
    uint8_t *input;
    uint8_t *wire;
};

/* Проверить ключ трансляции и параметры до расчёта размеров/выделения памяти. */
int stream_descriptor_validate(const struct stream_descriptor *d) {
    if (!d || !d->group_id || !stream_address_set(d->source) || !d->source_epoch || !d->n || d->r >= d->n
        || d->block_size <= RS_HEADER_SIZE || d->block_size > RS_MAX_BLOCK_SIZE
        || (size_t)(d->n - d->r) * (d->block_size - RS_HEADER_SIZE) <= STREAM_BATCH_OVERHEAD) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "descriptor invalid: present=%d group=%llx source=%s epoch=%llx n=%u r=%u size=%u",
                    d != NULL, d ? (unsigned long long)d->group_id : 0, d ? sc_public_key_hex(d->source.ed, (char[65]){0}) : "-",
                    d ? (unsigned long long)d->source_epoch : 0, d ? d->n : 0, d ? d->r : 0, d ? d->block_size : 0);
        return STREAM_ERR_ARGUMENT;
    }
    return STREAM_OK;
}

size_t stream_block_capacity(const struct stream_descriptor *d) {
    return stream_descriptor_validate(d) == STREAM_OK ? d->block_size + STREAM_BLOCK_OVERHEAD : 0;
}

size_t stream_input_size(const struct stream_descriptor *d) {
    return stream_descriptor_validate(d) == STREAM_OK
           ? (size_t)(d->n - d->r) * (d->block_size - RS_HEADER_SIZE) - STREAM_BATCH_OVERHEAD : 0;
}

/* Создать рабочую память RS и получить публичный ключ именно переданного Ed25519 seed. */
int stream_encoder_create(struct stream_encoder **out, const struct stream_descriptor *d, const uint8_t private_key[32]) {
    if (!out || *out || !private_key || stream_descriptor_validate(d) != STREAM_OK ||
        !stream_address_set(stream_address_read(d->group_key))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encoder create invalid arguments out=%p key=%p", (void *)out, (const void *)private_key);
        return STREAM_ERR_ARGUMENT;
    }
    struct stream_encoder *encoder = u_calloc(1, sizeof(*encoder));
    if (!encoder) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encoder allocation failed source=%s", sc_public_key_hex(d->source.ed, (char[65]){0}));
        return STREAM_ERR_MEMORY;
    }
    encoder->descriptor = *d;
    memcpy(encoder->private_key, private_key, sizeof(encoder->private_key));
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, SC_PRIVKEY_SIZE);
    size_t public_length = SC_PUBKEY_SIZE;
    int key_ok = key && EVP_PKEY_get_raw_public_key(key, encoder->public_key, &public_length) == 1 && public_length == SC_PUBKEY_SIZE;
    EVP_PKEY_free(key);
    if (!key_ok || memcmp(d->source.ed, encoder->public_key, SC_PUBKEY_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encoder Ed25519 key initialization failed source=%s",
                    sc_public_key_hex(d->source.ed, (char[65]){0}));
        stream_encoder_destroy(encoder);
        return STREAM_ERR_CRYPTO;
    }
    if (rs_init(&encoder->rs, d->n, d->r, d->block_size) != RS_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encoder RS initialization failed n=%u r=%u size=%u", d->n, d->r, d->block_size);
        stream_encoder_destroy(encoder);
        return STREAM_ERR_FEC;
    }
    encoder->input = u_malloc((uint32_t)rs_input_capacity(encoder->rs));
    encoder->wire = u_malloc((uint32_t)(d->n * stream_block_capacity(d)));
    if (!encoder->input || !encoder->wire) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encoder working memory allocation failed n=%u size=%u", d->n, d->block_size);
        stream_encoder_destroy(encoder);
        return STREAM_ERR_MEMORY;
    }
    *out = encoder;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "encoder created group=%llx source=%s epoch=%llx FEC=%u+%u block=%zu input=%zu",
               (unsigned long long)d->group_id, sc_public_key_hex(d->source.ed, (char[65]){0}), (unsigned long long)d->source_epoch,
               d->n - d->r, d->r, stream_block_capacity(d), stream_input_size(d));
    return STREAM_OK;
}

/* Очистить секретный seed и рабочее содержимое до возврата памяти allocator. */
void stream_encoder_destroy(struct stream_encoder *encoder) {
    if (!encoder) return;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "encoder destroy source=%s epoch=%llx",
               sc_public_key_hex(encoder->descriptor.source.ed, (char[65]){0}), (unsigned long long)encoder->descriptor.source_epoch);
    OPENSSL_cleanse(encoder->private_key, sizeof(encoder->private_key));
    if (encoder->input) OPENSSL_cleanse(encoder->input, rs_input_capacity(encoder->rs));
    u_free(encoder->input);
    u_free(encoder->wire);
    rs_free(encoder->rs);
    u_free(encoder);
}

const uint8_t *stream_encoder_public_key(const struct stream_encoder *encoder) {
    if (!encoder) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encoder public key: null context"); return NULL; }
    return encoder->public_key;
}

/* Подписать метаданные и содержимое один раз, затем защитить всё сообщение кодированием RS. */
int stream_encoder_encode(struct stream_encoder *encoder, uint64_t seq, uint16_t timestamp_ms, uint8_t flags,
                          const uint8_t *input, size_t length, const uint8_t **blocks, size_t *block_length) {
    if (!encoder || !input || !blocks || !block_length || !length || seq == UINT64_MAX || (flags & ~STREAM_BLOCK_TIME_RESET)
        || length > stream_input_size(&encoder->descriptor)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encode invalid arguments seq=%llu len=%zu flags=%u", (unsigned long long)seq, length, flags);
        return STREAM_ERR_ARGUMENT;
    }
    const struct stream_descriptor *d = &encoder->descriptor;
    size_t signed_size = STREAM_BATCH_HEADER + length, batch_size = signed_size + SC_SIGN_SIZE;
    size_t rs_size = RS_HEADER_SIZE + (batch_size + d->n - d->r - 1) / (d->n - d->r);
    size_t wire_size = STREAM_BLOCK_OVERHEAD + rs_size, stride = stream_block_capacity(d);
    uint8_t *rs_blocks[RS_MAX_BLOCKS];
    uint8_t *batch = encoder->input;
    memcpy(batch, STREAM_BATCH_DOMAIN, STREAM_BLOCK_DOMAIN_SIZE);
    memcpy(batch + STREAM_BLOCK_GROUP_OFF, d->group_key, SC_PUBKEY_SIZE);
    memcpy(batch + STREAM_BLOCK_SOURCE_OFF, d->source.ed, SC_PUBKEY_SIZE);
    stream_write_be(batch + STREAM_BLOCK_EPOCH_OFF, d->source_epoch, 8);
    batch[STREAM_BLOCK_N_OFF] = d->n; batch[STREAM_BLOCK_R_OFF] = d->r;
    stream_write_be(batch + STREAM_BLOCK_SIZE_OFF, rs_size, 2);
    stream_write_be(batch + STREAM_BLOCK_TIMESTAMP_OFF, timestamp_ms, 2);
    stream_write_be(batch + STREAM_BLOCK_LENGTH_OFF, length, 4);
    batch[STREAM_BLOCK_FLAGS_OFF] = flags;
    stream_write_be(batch + STREAM_BATCH_SEQ_OFF, seq, 8);
    memcpy(batch + STREAM_BATCH_HEADER, input, length);
    if (sc_ed25519_sign(encoder->private_key, batch, signed_size, batch + signed_size) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encode batch signature failed source=%s seq=%llu length=%zu",
                    sc_public_key_hex(d->source.ed, (char[65]){0}), (unsigned long long)seq, length);
        return STREAM_ERR_CRYPTO;
    }
    for (unsigned i = 0; i < d->n; i++) {
        uint8_t *wire = encoder->wire + i * stride;
        memcpy(wire, batch, STREAM_BLOCK_RS_OFF);
        memcpy(wire, STREAM_BLOCK_DOMAIN, STREAM_BLOCK_DOMAIN_SIZE);
        rs_blocks[i] = wire + STREAM_BLOCK_RS_OFF;
    }
    size_t encoded_size = 0;
    if (rs_encode(encoder->rs, seq, batch, batch_size, rs_blocks, &encoded_size) != RS_OK || encoded_size != rs_size) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "encode RS failed seq=%llu", (unsigned long long)seq);
        return STREAM_ERR_FEC;
    }
    for (unsigned i = 0; i < d->n; i++) blocks[i] = encoder->wire + i * stride;
    *block_length = wire_size;
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "encoded signed batch seq=%llu timestamp=%u len=%zu n=%u flags=%u part=%zu max=%zu padding=%zu",
                (unsigned long long)seq, timestamp_ms, length, d->n, flags, wire_size, stride,
                (size_t)(d->n - d->r) * (rs_size - RS_HEADER_SIZE) - batch_size);
    return STREAM_OK;
}

/* Только структурная проверка до хранения/пересылки; подлинность определяет восстановленная пачка. */
int stream_block_parse(const struct stream_descriptor *d, const uint8_t *wire, size_t length, struct stream_block_view *view) {
    if (!wire || !view || stream_descriptor_validate(d) != STREAM_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "part parse invalid arguments length=%zu", length);
        return STREAM_ERR_ARGUMENT;
    }
    if (length <= STREAM_BLOCK_OVERHEAD + RS_HEADER_SIZE || length > stream_block_capacity(d)
        || memcmp(wire, STREAM_BLOCK_DOMAIN, STREAM_BLOCK_DOMAIN_SIZE)
        || memcmp(wire + STREAM_BLOCK_GROUP_OFF, d->group_key, SC_PUBKEY_SIZE)
        || memcmp(wire + STREAM_BLOCK_SOURCE_OFF, d->source.ed, SC_PUBKEY_SIZE)
        || stream_read_be(wire + STREAM_BLOCK_EPOCH_OFF, 8) != d->source_epoch
        || wire[STREAM_BLOCK_N_OFF] != d->n || wire[STREAM_BLOCK_R_OFF] != d->r
        || stream_read_be(wire + STREAM_BLOCK_SIZE_OFF, 2) != length - STREAM_BLOCK_OVERHEAD) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "part format/descriptor mismatch source=%s epoch=%llx length=%zu max=%zu",
                   sc_public_key_hex(d->source.ed, (char[65]){0}), (unsigned long long)d->source_epoch, length, stream_block_capacity(d));
        return STREAM_ERR_FORMAT;
    }
    struct stream_block_view parsed = {
        .batch = {.seq = stream_read_be(wire + STREAM_BLOCK_SEQ_OFF, 8),
            .data_length = (uint32_t)stream_read_be(wire + STREAM_BLOCK_LENGTH_OFF, 4),
            .timestamp_ms = (uint16_t)stream_read_be(wire + STREAM_BLOCK_TIMESTAMP_OFF, 2),
            .block_size = (uint16_t)(length - STREAM_BLOCK_OVERHEAD), .flags = wire[STREAM_BLOCK_FLAGS_OFF]},
        .subblock = wire[STREAM_BLOCK_SUB_OFF], .rs = wire + STREAM_BLOCK_RS_OFF
    };
    const struct stream_batch_info *info = &parsed.batch;
    if (info->seq == UINT64_MAX || parsed.subblock >= d->n || !info->data_length || info->data_length > stream_input_size(d)
        || (info->flags & ~STREAM_BLOCK_TIME_RESET)
        || info->block_size != RS_HEADER_SIZE + (STREAM_BATCH_OVERHEAD + info->data_length + d->n - d->r - 1) / (d->n - d->r)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "part invalid metadata seq=%llu sub=%u length=%u flags=%u size=%u",
                   (unsigned long long)info->seq, parsed.subblock, info->data_length, info->flags, info->block_size);
        return STREAM_ERR_FORMAT;
    }
    *view = parsed;
    return STREAM_OK;
}

/* Подпись проверяется после RS; внешние подсказки обязаны совпадать с подписанными полями пачки. */
int stream_batch_verify(const struct stream_descriptor *d, const uint8_t public_key[32],
                        const struct stream_batch_info *expected, const uint8_t *decoded, size_t decoded_length) {
    if (!public_key || !expected || !decoded || stream_descriptor_validate(d) != STREAM_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "batch verify invalid arguments length=%zu", decoded_length);
        return STREAM_ERR_ARGUMENT;
    }
    if (expected->seq == UINT64_MAX || !expected->data_length || expected->data_length > stream_input_size(d)
        || (expected->flags & ~STREAM_BLOCK_TIME_RESET) || expected->block_size > d->block_size
        || expected->block_size != RS_HEADER_SIZE + (STREAM_BATCH_OVERHEAD + expected->data_length + d->n - d->r - 1) / (d->n - d->r)
        || decoded_length != (size_t)(d->n - d->r) * (expected->block_size - RS_HEADER_SIZE)
        || decoded_length < STREAM_BATCH_OVERHEAD + expected->data_length) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "batch invalid size source=%s seq=%llu decoded=%zu data=%u part=%u",
                   sc_public_key_hex(d->source.ed, (char[65]){0}), (unsigned long long)expected->seq,
                   decoded_length, expected->data_length, expected->block_size);
        return STREAM_ERR_FORMAT;
    }
    size_t signed_size = STREAM_BATCH_HEADER + expected->data_length;
    if (memcmp(public_key, d->source.ed, SC_PUBKEY_SIZE) || sc_ed25519_verify(public_key, decoded, signed_size, decoded + signed_size) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "batch source signature rejected source=%s epoch=%llx seq=%llu data=%u part=%u",
                   sc_public_key_hex(d->source.ed, (char[65]){0}), (unsigned long long)d->source_epoch,
                   (unsigned long long)expected->seq, expected->data_length, expected->block_size);
        return STREAM_ERR_SIGNATURE;
    }
    if (memcmp(decoded, STREAM_BATCH_DOMAIN, STREAM_BLOCK_DOMAIN_SIZE)
        || memcmp(decoded + STREAM_BLOCK_GROUP_OFF, d->group_key, SC_PUBKEY_SIZE)
        || memcmp(decoded + STREAM_BLOCK_SOURCE_OFF, d->source.ed, SC_PUBKEY_SIZE)
        || stream_read_be(decoded + STREAM_BLOCK_EPOCH_OFF, 8) != d->source_epoch
        || decoded[STREAM_BLOCK_N_OFF] != d->n || decoded[STREAM_BLOCK_R_OFF] != d->r
        || stream_read_be(decoded + STREAM_BLOCK_SIZE_OFF, 2) != expected->block_size
        || stream_read_be(decoded + STREAM_BLOCK_TIMESTAMP_OFF, 2) != expected->timestamp_ms
        || stream_read_be(decoded + STREAM_BLOCK_LENGTH_OFF, 4) != expected->data_length
        || decoded[STREAM_BLOCK_FLAGS_OFF] != expected->flags
        || stream_read_be(decoded + STREAM_BATCH_SEQ_OFF, 8) != expected->seq) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "batch signed metadata mismatch source=%s epoch=%llx seq=%llu",
                   sc_public_key_hex(d->source.ed, (char[65]){0}), (unsigned long long)d->source_epoch, (unsigned long long)expected->seq);
        return STREAM_ERR_FORMAT;
    }
    for (size_t i = signed_size + SC_SIGN_SIZE; i < decoded_length; i++) {
        if (decoded[i]) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "batch nonzero padding source=%s seq=%llu offset=%zu",
                       sc_public_key_hex(d->source.ed, (char[65]){0}), (unsigned long long)expected->seq, i);
            return STREAM_ERR_FORMAT;
        }
    }
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "batch source signature verified source=%s seq=%llu length=%u part=%u",
                sc_public_key_hex(d->source.ed, (char[65]){0}), (unsigned long long)expected->seq, expected->data_length, expected->block_size);
    return STREAM_OK;
}
