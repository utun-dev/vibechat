#include <string.h>

#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "stream_source.h"

struct stream_source {
    struct UASYNC *ua;
    struct stream_descriptor descriptor;
    struct stream_encoder *encoder;
    struct stream_buffer *buffer;
    uint8_t *input;
    size_t capacity;
    size_t used;
    uint64_t next_seq;
    uint64_t last_batch_ms;
    uint64_t emitted_bytes;
    uint64_t report_ms;
    void *timer;
    stream_source_ready_fn ready;
    void *arg;
    int busy;
    int has_batch;
    int failed;
    int ended;
    int write_mode; /* 0 — не выбран, 1 — байты, 2 — целые пачки. */
};

/* Ошибка не скрывает уже принятый префикс: прекращаем таймер и дальнейшие write. */
static int source_fail(struct stream_source *source, int result) {
    source->failed = 1;
    if (source->timer) { uasync_cancel_timeout(source->ua, source->timer); source->timer = NULL; }
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source failed source=%s seq=%llu pending=%zu result=%d",
                STREAM_ADDRESS_TEXT(source->descriptor.source), (unsigned long long)source->next_seq, source->used, result);
    return result;
}

/* Сформировать блоки и сохранить их до уведомления потребителей; busy установлен внешней операцией. */
static int source_flush(struct stream_source *source) {
    if (!source->used) return STREAM_OK;
    if (source->timer) { uasync_cancel_timeout(source->ua, source->timer); source->timer = NULL; }
    uint64_t now_ms = get_time_tb() / 10;
    if (source->next_seq == UINT64_MAX || (source->has_batch && now_ms < source->last_batch_ms)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source sequence/clock invalid seq=%llu now=%llu previous=%llu",
                    (unsigned long long)source->next_seq, (unsigned long long)now_ms, (unsigned long long)source->last_batch_ms);
        return source_fail(source, STREAM_ERR_STATE);
    }
    uint8_t flags = source->has_batch && now_ms - source->last_batch_ms >= STREAM_TIMESTAMP_PERIOD_MS ? STREAM_BLOCK_TIME_RESET : 0;
    const uint8_t *blocks[RS_MAX_BLOCKS];
    size_t block_length = 0;
    int result = stream_encoder_encode(source->encoder, source->next_seq, (uint16_t)now_ms, flags,
                                       source->input, source->used, blocks, &block_length);
    if (result != STREAM_OK) return source_fail(source, result);
    for (unsigned i = 0; i < source->descriptor.n; i++) {
        result = stream_buffer_put(source->buffer, blocks[i], block_length);
        if (result != STREAM_OK) return source_fail(source, result < 0 ? result : STREAM_ERR_STATE);
    }
    uint64_t seq = source->next_seq++;
    source->emitted_bytes += source->used;
    source->used = 0;
    source->has_batch = 1;
    source->last_batch_ms = now_ms;
    if (!source->report_ms || now_ms - source->report_ms >= 1000) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "source progress source=%s seq=%llu bytes=%llu timestamp=%u flags=%u part=%zu max=%zu",
                    STREAM_ADDRESS_TEXT(source->descriptor.source), (unsigned long long)seq,
                    (unsigned long long)source->emitted_bytes, (uint16_t)now_ms, flags,
                    block_length, stream_block_capacity(&source->descriptor));
        source->report_ms = now_ms;
    }
    if (source->ready) source->ready(source, seq, source->arg);
    return STREAM_OK;
}

/* Срабатывание единственного таймера неполной пачки; handle больше не принадлежит таймерному heap. */
static void source_flush_timer(void *arg) {
    struct stream_source *source = arg;
    source->timer = NULL;
    source->busy = 1;
    int result = source_flush(source);
    source->busy = 0;
    if (result != STREAM_OK) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source timed flush failed result=%d", result);
}

/* Создать только реальную локальную публикацию; память удалённых анонсов сюда не попадает. */
int stream_source_create(struct stream_source **out, struct UASYNC *ua, const struct stream_descriptor *d,
                         const uint8_t private_key[32], stream_source_ready_fn ready, void *arg) {
    if (!out || *out || !ua || !private_key || stream_descriptor_validate(d) != STREAM_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source create invalid arguments out=%p ua=%p", (void *)out, (void *)ua);
        return STREAM_ERR_ARGUMENT;
    }
    struct stream_source *source = u_calloc(1, sizeof(*source));
    if (!source) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source context allocation failed"); return STREAM_ERR_MEMORY; }
    source->ua = ua;
    source->descriptor = *d;
    source->ready = ready;
    source->arg = arg;
    source->capacity = stream_input_size(d);
    source->input = u_malloc((uint32_t)source->capacity);
    int result = source->input ? stream_encoder_create(&source->encoder, d, private_key) : STREAM_ERR_MEMORY;
    if (result == STREAM_OK)
        result = stream_buffer_create(&source->buffer, d, stream_encoder_public_key(source->encoder), STREAM_BUFFER_BYTES);
    if (result != STREAM_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source initialization failed source=%s result=%d", STREAM_ADDRESS_TEXT(d->source), result);
        stream_source_destroy(source);
        return result;
    }
    *out = source;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "source created group=%llx source=%s epoch=%llx input=%zu flush=100ms",
               (unsigned long long)d->group_id, STREAM_ADDRESS_TEXT(d->source),
               (unsigned long long)d->source_epoch, source->capacity);
    return STREAM_OK;
}

void stream_source_destroy(struct stream_source *source) {
    if (!source) return;
    if (source->busy) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source destroy inside callback rejected"); return; }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "source destroy source=%s batches=%llu bytes=%llu pending=%zu ended=%d failed=%d",
               STREAM_ADDRESS_TEXT(source->descriptor.source), (unsigned long long)source->next_seq,
               (unsigned long long)source->emitted_bytes, source->used, source->ended, source->failed);
    if (source->timer) uasync_cancel_timeout(source->ua, source->timer);
    stream_buffer_destroy(source->buffer);
    stream_encoder_destroy(source->encoder);
    u_free(source->input);
    u_free(source);
}

size_t stream_source_input_capacity(const struct stream_source *source) {
    if (!source) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source capacity null context"); return 0; }
    return source->capacity;
}

struct stream_buffer *stream_source_buffer(struct stream_source *source) {
    if (!source) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source buffer null context"); return NULL; }
    return source->buffer;
}

/* Принимать ограниченную порцию, дополнять старую пачку и заводить таймер только на новый остаток. */
int stream_source_write(struct stream_source *source, const uint8_t *data, size_t length, size_t *accepted) {
    if (accepted) *accepted = 0;
    if (!source || !accepted || (length && !data) || length > source->capacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source write invalid arguments length=%zu", length);
        return STREAM_ERR_ARGUMENT;
    }
    if (source->busy || source->failed || source->ended) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "source write state rejected busy=%d failed=%d ended=%d", source->busy, source->failed, source->ended);
        return STREAM_ERR_STATE;
    }
    if (!length) return STREAM_OK;
    if (source->write_mode == 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source byte write conflicts with block mode length=%zu", length);
        return STREAM_ERR_ARGUMENT;
    }
    source->write_mode = 1;
    source->busy = 1;
    int result = STREAM_OK;
    while (*accepted < length) {
        size_t take = source->capacity - source->used;
        if (take > length - *accepted) take = length - *accepted;
        memcpy(source->input + source->used, data + *accepted, take);
        source->used += take;
        *accepted += take;
        if (source->used == source->capacity && (result = source_flush(source)) != STREAM_OK) break;
    }
    if (result == STREAM_OK && source->used && !source->timer) {
        source->timer = uasync_set_timeout(source->ua, STREAM_SOURCE_FLUSH_TB, source, source_flush_timer, "stream_source_flush");
        if (!source->timer) result = source_fail(source, STREAM_ERR_MEMORY);
    }
    source->busy = 0;
    return result;
}

/* Finish отменяет таймер и публикует остаток сразу; дальнейшие байты требуют нового source_epoch. */
int stream_source_finish(struct stream_source *source) {
    if (!source || source->busy || source->failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source finish invalid state present=%d", source != NULL);
        return source ? STREAM_ERR_STATE : STREAM_ERR_ARGUMENT;
    }
    if (source->ended) return STREAM_OK;
    source->busy = 1;
    int result = source_flush(source);
    source->busy = 0;
    if (result == STREAM_OK) {
        source->ended = 1;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "source finished source=%s batches=%llu bytes=%llu",
                   STREAM_ADDRESS_TEXT(source->descriptor.source), (unsigned long long)source->next_seq,
                   (unsigned long long)source->emitted_bytes);
    }
    return result;
}

/* Сохранить границу медиаблока даже для неполной пачки; общий flush подписывает и сохраняет все подблоки. */
int stream_source_write_block(struct stream_source *source, const uint8_t *data, size_t length) {
    if (!source || !data || !length || length > source->capacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source block write invalid length=%zu capacity=%zu", length, source ? source->capacity : 0);
        return STREAM_ERR_ARGUMENT;
    }
    if (source->busy || source->failed || source->ended) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source block write rejected busy=%d failed=%d ended=%d",
                    source->busy, source->failed, source->ended);
        return STREAM_ERR_STATE;
    }
    if (source->write_mode == 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source block write conflicts with byte mode pending=%zu", source->used);
        return STREAM_ERR_ARGUMENT;
    }
    source->write_mode = 2;
    source->busy = 1;
    memcpy(source->input, data, length);
    source->used = length;
    int result = source_flush(source);
    source->busy = 0;
    return result;
}
