#include <string.h>

#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "stream_buffer.h"

#define STREAM_SLOT_NONE UINT32_MAX

struct stream_cached_path { uint8_t count; struct stream_address nodes[]; };

struct stream_cached_slot {
    uint32_t next;
    uint8_t subblock;
    struct stream_cached_path *path; /* NULL — исходный путь {source}, без отдельной аллокации. */
    uint32_t sub_next, sub_previous;
    uint64_t seq, received_ms;
};

struct stream_cached_batch {
    struct stream_cached_batch *hash_next;
    uint64_t seq;
    uint32_t first_slot;
    uint32_t data_length;
    uint16_t timestamp_ms;
    uint16_t block_size;
    uint16_t count;
    uint8_t flags;
    int rejected;
    int verified;
    uint8_t verified_parts[(RS_MAX_BLOCKS + 7) / 8];
    struct stream_frame_timing timing; // Подсказка для очередей relay без восстановления FEC
};

struct stream_buffer {
    struct stream_descriptor descriptor;
    uint8_t public_key[SC_PUBKEY_SIZE];
    struct rs_codec *rs;
    uint8_t *decode_output;
    uint8_t *data;
    struct stream_cached_slot *slots;
    struct stream_cached_batch **hash;
    struct stream_cached_batch **heap;
    uint64_t latest[RS_MAX_BLOCKS];
    uint8_t latest_valid[RS_MAX_BLOCKS];
    struct stream_telemetry *telemetry[RS_MAX_BLOCKS]; // Последнее измерение каждого получаемого субпотока
    uint32_t frame_period_us;
    size_t block_bytes;
    uint32_t capacity;
    uint32_t hash_size;
    uint32_t free_slot;
    struct stream_buffer_stats stats;
    uint32_t sub_head[RS_MAX_BLOCKS], sub_tail[RS_MAX_BLOCKS];
    stream_buffer_event_fn observer;
    void *observer_arg;
};

/* Индекс пачки не зависит от порядка поступления; множество дырок не требует линейного поиска. */
static uint32_t buffer_hash(const struct stream_buffer *b, uint64_t seq) {
    seq ^= seq >> 33;
    seq *= UINT64_C(0xff51afd7ed558ccd);
    seq ^= seq >> 33;
    return (uint32_t)seq & (b->hash_size - 1);
}

static struct stream_cached_batch *buffer_batch(const struct stream_buffer *b, uint64_t seq) {
    for (struct stream_cached_batch *batch = b->hash[buffer_hash(b, seq)]; batch; batch = batch->hash_next)
        if (batch->seq == seq) return batch;
    return NULL;
}

/* Поиск не более n блоков пачки; пустые субпотоки не занимают отдельные позиции в массиве данных. */
static uint32_t buffer_slot(const struct stream_buffer *b, const struct stream_cached_batch *batch, uint8_t subblock) {
    for (uint32_t slot = batch->first_slot; slot != STREAM_SLOT_NONE; slot = b->slots[slot].next)
        if (b->slots[slot].subblock == subblock) return slot;
    return STREAM_SLOT_NONE;
}

/* Обмен узлов min-heap, упорядоченного по индексу пачки. */
static void buffer_heap_swap(struct stream_buffer *b, uint32_t first, uint32_t second) {
    struct stream_cached_batch *batch = b->heap[first];
    b->heap[first] = b->heap[second];
    b->heap[second] = batch;
}

/* Добавить новую пачку в хеш и heap; память уже выделена, отказов здесь нет. */
static void buffer_insert_batch(struct stream_buffer *b, struct stream_cached_batch *batch) {
    uint32_t hash = buffer_hash(b, batch->seq);
    batch->hash_next = b->hash[hash];
    b->hash[hash] = batch;
    uint32_t index = b->stats.batches++;
    b->heap[index] = batch;
    while (index && b->heap[(index - 1) / 2]->seq > batch->seq) {
        buffer_heap_swap(b, index, (index - 1) / 2);
        index = (index - 1) / 2;
    }
}

/* Удалить целую минимальную пачку; выданные наружу указатели становятся недействительными. */
static void buffer_evict_oldest(struct stream_buffer *b) {
    struct stream_cached_batch *batch = b->heap[0];
    uint32_t hash = buffer_hash(b, batch->seq);
    struct stream_cached_batch **link = &b->hash[hash];
    while (*link != batch) link = &(*link)->hash_next;
    *link = batch->hash_next;
    uint32_t remaining = --b->stats.batches;
    if (remaining) {
        b->heap[0] = b->heap[remaining];
        uint32_t index = 0;
        while (index * 2 + 1 < remaining) {
            uint32_t child = index * 2 + 1;
            if (child + 1 < remaining && b->heap[child + 1]->seq < b->heap[child]->seq) child++;
            if (b->heap[index]->seq <= b->heap[child]->seq) break;
            buffer_heap_swap(b, index, child);
            index = child;
        }
    }
    for (uint32_t slot = batch->first_slot; slot != STREAM_SLOT_NONE;) {
        uint32_t next = b->slots[slot].next;
        unsigned sub = b->slots[slot].subblock;
        uint32_t before = b->slots[slot].sub_previous, after = b->slots[slot].sub_next;
        if (before == STREAM_SLOT_NONE) b->sub_head[sub] = after; else b->slots[before].sub_next = after;
        if (after == STREAM_SLOT_NONE) b->sub_tail[sub] = before; else b->slots[after].sub_previous = before;
        if (b->observer) b->observer(b->observer_arg, b->data + slot * b->block_bytes, STREAM_BLOCK_OVERHEAD + batch->block_size, 0);
        u_free(b->slots[slot].path); b->slots[slot].path = NULL;
        if (b->latest_valid[sub] && b->latest[sub] == batch->seq) b->latest_valid[sub] = 0;
        b->slots[slot].next = b->free_slot;
        b->free_slot = slot;
        slot = next;
    }
    b->stats.expired_before = batch->seq + 1;
    b->stats.blocks -= batch->count;
    b->stats.stored_bytes -= batch->count * (STREAM_BLOCK_OVERHEAD + batch->block_size);
    b->stats.evicted_batches++;
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "buffer evicted seq=%llu blocks=%u retained=%u bytes=%zu low=%llu",
                (unsigned long long)batch->seq, batch->count, b->stats.blocks, b->stats.stored_bytes,
                (unsigned long long)b->stats.expired_before);
    u_free(batch);
}

/* Выделить ограниченные массивы данных/индексов; память пачек растёт только по фактическому поступлению. */
int stream_buffer_create(struct stream_buffer **out, const struct stream_descriptor *d, const uint8_t public_key[32], size_t bytes) {
    if (!out || *out || !public_key || stream_descriptor_validate(d) != STREAM_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer create invalid arguments out=%p key=%p", (void *)out, (const void *)public_key);
        return STREAM_ERR_ARGUMENT;
    }
    size_t block_bytes = stream_block_capacity(d);
    if (!bytes) bytes = STREAM_BUFFER_BYTES;
    if (bytes > STREAM_BUFFER_BYTES || bytes < d->n * block_bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer budget invalid bytes=%zu batch=%zu max=%u", bytes, d->n * block_bytes, STREAM_BUFFER_BYTES);
        return STREAM_ERR_ARGUMENT;
    }
    struct stream_buffer *b = u_calloc(1, sizeof(*b));
    if (!b) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer context allocation failed bytes=%zu", bytes); return STREAM_ERR_MEMORY; }
    b->descriptor = *d;
    memcpy(b->public_key, public_key, SC_PUBKEY_SIZE);
    b->block_bytes = block_bytes;
    b->capacity = (uint32_t)(bytes / block_bytes);
    b->hash_size = 1;
    while (b->hash_size < b->capacity) b->hash_size *= 2;
    b->stats.capacity_bytes = b->capacity * block_bytes;
    b->data = u_malloc((uint32_t)b->stats.capacity_bytes);
    b->slots = u_calloc(b->capacity, sizeof(*b->slots));
    b->heap = u_calloc(b->capacity, sizeof(*b->heap));
    b->hash = u_calloc(b->hash_size, sizeof(*b->hash));
    b->decode_output = u_malloc((uint32_t)(stream_input_size(d) + STREAM_BATCH_OVERHEAD));
    if (!b->data || !b->slots || !b->heap || !b->hash || !b->decode_output) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer arrays allocation failed capacity=%u bytes=%zu", b->capacity, b->stats.capacity_bytes);
        stream_buffer_destroy(b);
        return STREAM_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < b->capacity; i++) b->slots[i].next = i + 1;
    b->slots[b->capacity - 1].next = STREAM_SLOT_NONE;
    for (unsigned i = 0; i < RS_MAX_BLOCKS; i++) b->sub_head[i] = b->sub_tail[i] = STREAM_SLOT_NONE;
    if (rs_init(&b->rs, d->n, d->r, d->block_size) != RS_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer decode RS initialization failed n=%u r=%u", d->n, d->r);
        stream_buffer_destroy(b);
        return STREAM_ERR_FEC;
    }
    *out = b;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "buffer created source=%s epoch=%llx bytes=%zu block=%zu slots=%u",
               STREAM_ADDRESS_TEXT(d->source), (unsigned long long)d->source_epoch,
               b->stats.capacity_bytes, b->block_bytes, b->capacity);
    return STREAM_OK;
}

/* Прекращение владельцем всех обращений предшествует destroy; кольцо ничего не знает о чужих NCD handles. */
void stream_buffer_destroy(struct stream_buffer *b) {
    if (!b) return;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "buffer destroy source=%s blocks=%u batches=%u bytes=%zu evicted=%llu duplicates=%llu rejected=%llu",
               STREAM_ADDRESS_TEXT(b->descriptor.source), b->stats.blocks, b->stats.batches, b->stats.stored_bytes,
               (unsigned long long)b->stats.evicted_batches, (unsigned long long)b->stats.duplicates,
               (unsigned long long)b->stats.rejected_batches);
    for (uint32_t i = 0; i < b->stats.batches; i++) u_free(b->heap[i]);
    if (b->slots) for (uint32_t i = 0; i < b->capacity; i++) u_free(b->slots[i].path);
    for (unsigned i = 0; i < RS_MAX_BLOCKS; i++) u_free(b->telemetry[i]);
    rs_free(b->rs);
    u_free(b->decode_output);
    u_free(b->data);
    u_free(b->slots);
    u_free(b->heap);
    u_free(b->hash);
    u_free(b);
}

/* Сохранить часть после проверки формата; подпись источника проверяется при восстановлении пачки. */
int stream_buffer_put(struct stream_buffer *b, const uint8_t *wire, size_t length) {
    struct stream_path path = {.count = 1};
    if (b) path.nodes[0] = b->descriptor.source;
    return stream_buffer_put_path(b, wire, length, &path);
}

/* Путь и память готовятся до вытеснения: отказ не повреждает окно кольца. */
int stream_buffer_put_path(struct stream_buffer *b, const uint8_t *wire, size_t length, const struct stream_path *path) {
    if (!b || !wire) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer put invalid arguments length=%zu", length); return STREAM_ERR_ARGUMENT; }
    if (!path || !path->count || path->count > STREAM_PATH_MAX
        || stream_path_validate(path, b->descriptor.source, path->nodes[path->count - 1], (struct stream_address){{0}}) != STREAM_OK)
        return STREAM_ERR_FORMAT;
    struct stream_block_view view;
    int result = stream_block_parse(&b->descriptor, wire, length, &view);
    if (result != STREAM_OK) return result;
    const struct stream_batch_info *info = &view.batch;
    struct stream_cached_batch *batch = buffer_batch(b, info->seq);
    if (batch && batch->block_size == info->block_size) {
        uint32_t slot = buffer_slot(b, batch, view.subblock);
        if (slot != STREAM_SLOT_NONE && !memcmp(b->data + slot * b->block_bytes, wire, length)) {
            b->stats.duplicates++;
            DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "buffer duplicate seq=%llu sub=%u", (unsigned long long)batch->seq, view.subblock);
            return STREAM_DUPLICATE;
        }
    }
    if (info->seq < b->stats.expired_before) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "buffer expired incoming seq=%llu sub=%u low=%llu full=%d",
                   (unsigned long long)info->seq, view.subblock, (unsigned long long)b->stats.expired_before, b->stats.blocks == b->capacity);
        return STREAM_EXPIRED;
    }
    if (batch && (batch->timestamp_ms != info->timestamp_ms || batch->data_length != info->data_length || batch->flags != info->flags
                  || batch->block_size != info->block_size
                  || buffer_slot(b, batch, view.subblock) != STREAM_SLOT_NONE)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "buffer part conflict seq=%llu sub=%u timestamp=%u/%u length=%u/%u flags=%u/%u size=%u/%u",
                   (unsigned long long)info->seq, view.subblock, batch->timestamp_ms, info->timestamp_ms,
                   batch->data_length, info->data_length, batch->flags, info->flags, batch->block_size, info->block_size);
        return STREAM_ERR_CONFLICT;
    }
    if (b->stats.blocks == b->capacity && info->seq <= b->heap[0]->seq) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "buffer full: incoming seq=%llu sub=%u cannot replace newer data oldest=%llu",
                   (unsigned long long)info->seq, view.subblock, (unsigned long long)b->heap[0]->seq);
        return STREAM_EXPIRED;
    }
    struct stream_cached_path *stored_path = path->count > 1 ? u_malloc(sizeof(*stored_path) + path->count * sizeof(path->nodes[0])) : NULL;
    if (path->count > 1 && !stored_path) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer path allocation failed seq=%llu", (unsigned long long)info->seq);
        return STREAM_ERR_MEMORY;
    }
    if (stored_path) { stored_path->count = path->count; memcpy(stored_path->nodes, path->nodes, path->count * sizeof(path->nodes[0])); }
    int new_batch = batch == NULL;
    if (new_batch) {
        batch = u_calloc(1, sizeof(*batch));
        if (!batch) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer batch allocation failed seq=%llu", (unsigned long long)info->seq);
            u_free(stored_path); return STREAM_ERR_MEMORY;
        }
        batch->seq = info->seq;
        batch->first_slot = STREAM_SLOT_NONE;
        batch->timestamp_ms = info->timestamp_ms;
        batch->data_length = info->data_length;
        batch->block_size = info->block_size;
        batch->flags = info->flags;
    }
    if (b->stats.blocks == b->capacity) buffer_evict_oldest(b);
    if (new_batch) buffer_insert_batch(b, batch);
    uint32_t slot = b->free_slot;
    b->free_slot = b->slots[slot].next;
    b->slots[slot].subblock = view.subblock;
    b->slots[slot].path = stored_path;
    b->slots[slot].seq = info->seq; b->slots[slot].received_ms = get_time_tb() / 10;
    b->slots[slot].sub_next = STREAM_SLOT_NONE; b->slots[slot].sub_previous = b->sub_tail[view.subblock];
    if (b->sub_tail[view.subblock] == STREAM_SLOT_NONE) b->sub_head[view.subblock] = slot;
    else b->slots[b->sub_tail[view.subblock]].sub_next = slot;
    b->sub_tail[view.subblock] = slot;
    b->slots[slot].next = batch->first_slot;
    batch->first_slot = slot;
    batch->count++;
    memcpy(b->data + slot * b->block_bytes, wire, length);
    b->stats.blocks++;
    b->stats.stored_bytes += length;
    if (info->seq > b->stats.highest_seq) b->stats.highest_seq = info->seq;
    if (!b->latest_valid[view.subblock] || info->seq > b->latest[view.subblock]) {
        b->latest[view.subblock] = info->seq;
        b->latest_valid[view.subblock] = 1;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "buffer stored seq=%llu sub=%u present=%u/%u bytes=%zu",
                (unsigned long long)info->seq, view.subblock, batch->count, b->descriptor.n, b->stats.stored_bytes);
    if (b->observer) b->observer(b->observer_arg, b->data + slot * b->block_bytes, length, 1);
    return STREAM_OK;
}

void stream_buffer_set_observer(struct stream_buffer *b, stream_buffer_event_fn observer, void *arg) {
    if (!b) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer observer null buffer"); return; }
    b->observer = observer; b->observer_arg = arg;
}

/* Список субпотока упорядочен по монотонному времени вставки, включая поступившие вне порядка индексы. */
int stream_buffer_pending(struct stream_buffer *b, uint64_t seq, uint8_t sub, uint64_t *count, uint64_t *bytes, uint64_t *oldest) {
    if (!b || sub >= b->descriptor.n || (!count && !bytes && !oldest)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer pending invalid arguments sub=%u", sub); return STREAM_ERR_ARGUMENT;
    }
    uint64_t found = 0, total_bytes = 0, first_ms = 0;
    for (uint32_t slot = b->sub_head[sub]; slot != STREAM_SLOT_NONE; slot = b->slots[slot].sub_next) {
        if (b->slots[slot].seq < seq) continue;
        if (!found) first_ms = b->slots[slot].received_ms;
        found++;
        total_bytes += STREAM_BLOCK_OVERHEAD + stream_read_be(b->data + slot * b->block_bytes + STREAM_BLOCK_SIZE_OFF, 2);
        if (!count && !bytes) break;
    }
    if (count) *count = found;
    if (bytes) *bytes = total_bytes;
    if (oldest) *oldest = first_ms;
    return found ? STREAM_OK : STREAM_WAIT;
}

/* Копия пути не держит указатели в переиспользуемом кольце. */
int stream_buffer_path(struct stream_buffer *b, uint64_t seq, uint8_t subblock, struct stream_path *path) {
    if (!b || !path || subblock >= b->descriptor.n || seq == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer path invalid arguments"); return STREAM_ERR_ARGUMENT;
    }
    struct stream_cached_batch *batch = buffer_batch(b, seq);
    uint32_t slot = batch ? buffer_slot(b, batch, subblock) : STREAM_SLOT_NONE;
    if (slot == STREAM_SLOT_NONE) return seq < b->stats.expired_before ? STREAM_EXPIRED : STREAM_GAP;
    *path = (struct stream_path){.nodes = {b->descriptor.source}, .count = 1};
    if (b->slots[slot].path) {
        path->count = b->slots[slot].path->count;
        memcpy(path->nodes, b->slots[slot].path->nodes, path->count * sizeof(path->nodes[0]));
    }
    return STREAM_OK;
}

/* Поиск используется только на дыре/вытеснении, обычная выдача читает точный индекс через hash. */
int stream_buffer_next_seq(struct stream_buffer *b, uint64_t seq, uint8_t subblock, uint64_t *next) {
    if (!b || !next || subblock >= b->descriptor.n) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer next invalid arguments"); return STREAM_ERR_ARGUMENT;
    }
    uint64_t found = UINT64_MAX;
    for (uint32_t i = 0; i < b->stats.batches; i++) {
        struct stream_cached_batch *batch = b->heap[i];
        if (batch->seq >= seq && batch->seq < found && buffer_slot(b, batch, subblock) != STREAM_SLOT_NONE) found = batch->seq;
    }
    if (found == UINT64_MAX) return STREAM_WAIT;
    *next = found; return STREAM_OK;
}

/* Найти точную позицию либо последний блок субпотока, не меняя его историю/срок хранения. */
int stream_buffer_read(struct stream_buffer *b, uint64_t seq, uint8_t subblock,
                       const uint8_t **wire, size_t *length, uint64_t *actual_seq) {
    if (!b || !wire || !length || !actual_seq || subblock >= b->descriptor.n) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer read invalid arguments seq=%llu sub=%u", (unsigned long long)seq, subblock);
        return STREAM_ERR_ARGUMENT;
    }
    if (seq == UINT64_MAX) {
        if (!b->latest_valid[subblock]) return STREAM_WAIT;
        seq = b->latest[subblock];
    }
    if (seq < b->stats.expired_before) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "buffer read expired seq=%llu low=%llu", (unsigned long long)seq,
                    (unsigned long long)b->stats.expired_before);
        return STREAM_EXPIRED;
    }
    if (!b->stats.blocks || seq > b->stats.highest_seq) return STREAM_WAIT;
    struct stream_cached_batch *batch = buffer_batch(b, seq);
    uint32_t slot = batch ? buffer_slot(b, batch, subblock) : STREAM_SLOT_NONE;
    if (slot == STREAM_SLOT_NONE) {
        DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "buffer read gap seq=%llu sub=%u", (unsigned long long)seq, subblock);
        return STREAM_GAP;
    }
    *wire = b->data + slot * b->block_bytes;
    *length = STREAM_BLOCK_OVERHEAD + batch->block_size;
    *actual_seq = seq;
    return STREAM_OK;
}

/* Границы наличия конкретного субпотока не обещают заполнения всех промежуточных индексов. */
int stream_buffer_range(struct stream_buffer *b, uint8_t subblock, uint64_t *first, uint64_t *last) {
    if (!b || !first || !last || subblock >= b->descriptor.n) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer range invalid sub=%u", subblock);
        return STREAM_ERR_ARGUMENT;
    }
    if (!b->latest_valid[subblock]) return STREAM_WAIT;
    uint64_t low = b->latest[subblock];
    for (uint32_t i = 0; i < b->stats.batches; i++) {
        struct stream_cached_batch *batch = b->heap[i];
        if (batch->seq < low && buffer_slot(b, batch, subblock) != STREAM_SLOT_NONE) low = batch->seq;
        if (low == b->heap[0]->seq) break;
    }
    *first = low;
    *last = b->latest[subblock];
    return STREAM_OK;
}

/* При GAP потребитель сохраняет все ещё декодируемые пачки, даже если выбранные upstream уже далеко впереди. */
int stream_buffer_next_decodable(struct stream_buffer *b, uint64_t seq, uint64_t *next) {
    if (!b || !next) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer next decodable invalid arguments"); return STREAM_ERR_ARGUMENT; }
    uint64_t found = UINT64_MAX;
    for (uint32_t i = 0; i < b->stats.batches; i++) {
        struct stream_cached_batch *batch = b->heap[i];
        if (batch->seq >= seq && batch->seq < found && batch->count >= b->descriptor.n - b->descriptor.r) found = batch->seq;
    }
    if (found == UINT64_MAX) return STREAM_WAIT;
    *next = found; return STREAM_OK;
}

/* RS пишет в собственный scratch; ошибка или недостающие блоки не повреждают выход потребителя. */
int stream_buffer_decode(struct stream_buffer *b, uint64_t seq, uint8_t *output, size_t capacity, size_t *length) {
    if (!b || !output || !length || seq == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer decode invalid arguments seq=%llu capacity=%zu", (unsigned long long)seq, capacity);
        return STREAM_ERR_ARGUMENT;
    }
    if (seq < b->stats.expired_before) return STREAM_EXPIRED;
    struct stream_cached_batch *batch = buffer_batch(b, seq);
    if (!batch) return !b->stats.blocks || seq > b->stats.highest_seq ? STREAM_WAIT : STREAM_GAP;
    if (batch->rejected) return STREAM_REJECTED;
    if (batch->count < b->descriptor.n - b->descriptor.r) return STREAM_WAIT;
    if (capacity < batch->data_length) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer decode output too small seq=%llu capacity=%zu needed=%u",
                    (unsigned long long)seq, capacity, batch->data_length);
        return STREAM_ERR_ARGUMENT;
    }
    const uint8_t *blocks[RS_MAX_BLOCKS] = {0};
    for (uint32_t slot = batch->first_slot; slot != STREAM_SLOT_NONE; slot = b->slots[slot].next) {
        unsigned sub = b->slots[slot].subblock;
        if (!batch->verified || (batch->verified_parts[sub / 8] & (1u << (sub % 8))))
            blocks[sub] = b->data + slot * b->block_bytes + STREAM_BLOCK_RS_OFF;
    }
    size_t decoded_size = (size_t)(b->descriptor.n - b->descriptor.r) * (batch->block_size - RS_HEADER_SIZE);
    if (rs_decode(b->rs, blocks, batch->block_size, b->decode_output, rs_input_capacity(b->rs)) != RS_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer decode RS failed seq=%llu present=%u", (unsigned long long)seq, batch->count);
        return STREAM_ERR_FEC;
    }
    struct stream_batch_info expected = {.seq = batch->seq, .data_length = batch->data_length,
        .timestamp_ms = batch->timestamp_ms, .block_size = batch->block_size, .flags = batch->flags};
    int result = batch->verified ? STREAM_OK
                 : stream_batch_verify(&b->descriptor, b->public_key, &expected, b->decode_output, decoded_size);
    if (result != STREAM_OK) {
        if (result != STREAM_ERR_SIGNATURE && result != STREAM_ERR_FORMAT) return result;
        batch->rejected = 1; b->stats.rejected_batches++;
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "buffer rejected batch group=%llx source=%s epoch=%llx seq=%llu present=%u required=%u result=%d",
                   (unsigned long long)b->descriptor.group_id, STREAM_ADDRESS_TEXT(b->descriptor.source),
                   (unsigned long long)b->descriptor.source_epoch, (unsigned long long)seq,
                   batch->count, b->descriptor.n - b->descriptor.r, result);
        unsigned used = 0;
        for (unsigned sub = 0; sub < b->descriptor.n && used < (unsigned)(b->descriptor.n - b->descriptor.r); sub++) {
            if (!blocks[sub]) continue;
            uint32_t slot = buffer_slot(b, batch, sub);
            const struct stream_cached_path *path = b->slots[slot].path;
            struct stream_address supplier = path && path->count > 1 ? path->nodes[path->count - 2] : b->descriptor.source;
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "buffer rejected batch contribution seq=%llu sub=%u supplier=%s part=%u",
                       (unsigned long long)seq, sub, STREAM_ADDRESS_TEXT(supplier), batch->block_size);
            used++;
        }
        return STREAM_REJECTED;
    }
    if (!batch->verified) {
        unsigned selected = 0;
        for (unsigned sub = 0; sub < b->descriptor.n && selected < (unsigned)(b->descriptor.n - b->descriptor.r); sub++) {
            if (!blocks[sub]) continue;
            batch->verified_parts[sub / 8] |= (uint8_t)(1u << (sub % 8)); selected++;
        }
        batch->verified = 1;
        DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "buffer authenticated immutable RS set source=%s seq=%llu parts=%u",
                    STREAM_ADDRESS_TEXT(b->descriptor.source), (unsigned long long)seq, selected);
    }
    memcpy(output, b->decode_output + STREAM_BATCH_HEADER, batch->data_length);
    *length = batch->data_length;
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "buffer decoded seq=%llu present=%u length=%u", (unsigned long long)seq, batch->count, batch->data_length);
    return STREAM_OK;
}

int stream_buffer_get_stats(const struct stream_buffer *b, struct stream_buffer_stats *stats) {
    if (!b || !stats) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "buffer stats invalid arguments"); return STREAM_ERR_ARGUMENT; }
    *stats = b->stats;
    return STREAM_OK;
}

int stream_buffer_set_timing(struct stream_buffer *b, uint64_t seq, const struct stream_frame_timing *timing) {
    if (!b || !timing) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream timing missing buffer/value"); return STREAM_ERR_ARGUMENT; }
    struct stream_cached_batch *batch = buffer_batch(b, seq);
    if (!batch) return STREAM_EXPIRED;
    if (batch->timing.valid) {
        /* Сравниваем wire, поскольку padding структуры не является данными протокола. */
        uint8_t a[STREAM_FRAME_TIMING_SIZE], c[STREAM_FRAME_TIMING_SIZE];
        stream_frame_timing_write(a, &batch->timing); stream_frame_timing_write(c, timing);
        if (memcmp(a, c, sizeof(a))) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream conflicting frame timing seq=%llu", (unsigned long long)seq);
            return STREAM_ERR_CONFLICT;
        }
    }
    batch->timing = *timing;
    if (timing->valid && timing->period_us) b->frame_period_us = timing->period_us;
    return STREAM_OK;
}

int stream_buffer_get_timing(struct stream_buffer *b, uint64_t seq, struct stream_frame_timing *timing) {
    if (!b || !timing) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream timing missing buffer/output"); return STREAM_ERR_ARGUMENT; }
    struct stream_cached_batch *batch = buffer_batch(b, seq);
    if (!batch) return STREAM_EXPIRED;
    *timing = batch->timing; return STREAM_OK;
}

/* Число начал плюс один начатый ранее кадр; десятки частей большого KEY не становятся десятками кадров. */
int stream_buffer_get_delay(struct stream_buffer *b, uint64_t seq, uint8_t sub, struct stream_buffer_delay *out) {
    if (!b || !out || sub >= b->descriptor.n) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream delay invalid sub=%u", sub); return STREAM_ERR_ARGUMENT;
    }
    struct stream_buffer_delay delay = {.known = 1};
    const struct stream_frame_timing *first = NULL, *last = NULL;
    uint64_t first_seq = UINT64_MAX, last_seq = 0;
    for (uint32_t slot = b->sub_head[sub]; slot != STREAM_SLOT_NONE; slot = b->slots[slot].sub_next) {
        if (b->slots[slot].seq < seq) continue;
        struct stream_cached_batch *batch = buffer_batch(b, b->slots[slot].seq);
        const struct stream_frame_timing *t = &batch->timing;
        if (!t->valid) { delay.known = 0; continue; }
        delay.audio_us += t->audio_us;
        if (!t->video) continue;
        delay.frames += t->started;
        if (!first || batch->seq < first_seq) { first = t; first_seq = batch->seq; }
        if (!last || batch->seq > last_seq) { last = t; last_seq = batch->seq; }
    }
    if (first) {
        delay.frames += first->continuation;
        uint32_t period = last->period_us ? last->period_us : b->frame_period_us;
        delay.first_pts = first->first_pts; delay.last_pts = last->last_pts; delay.period_us = period;
        if (!period || last->last_pts < first->first_pts) delay.known = 0;
        else delay.video_us = last->last_pts - first->first_pts + period;
    }
    *out = delay; return STREAM_OK;
}

int stream_buffer_set_telemetry(struct stream_buffer *b, uint8_t sub, const struct stream_telemetry *t) {
    if (!b || !t || sub >= b->descriptor.n || !t->count || t->count > STREAM_PATH_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream telemetry cache invalid sub=%u", sub); return STREAM_ERR_ARGUMENT;
    }
    if (!b->telemetry[sub]) b->telemetry[sub] = u_malloc(sizeof(*t));
    if (!b->telemetry[sub]) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream telemetry cache allocation failed"); return STREAM_ERR_MEMORY; }
    *b->telemetry[sub] = *t; return STREAM_OK;
}

int stream_buffer_get_telemetry(struct stream_buffer *b, uint8_t sub, uint64_t now, struct stream_telemetry *t) {
    if (!b || !t || sub >= b->descriptor.n) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream telemetry cache invalid output sub=%u", sub); return STREAM_ERR_ARGUMENT;
    }
    if (!b->telemetry[sub]) return STREAM_WAIT;
    *t = *b->telemetry[sub]; stream_telemetry_age(t, now); return STREAM_OK;
}

/* Потерян последний переход, доступность более ранних узлов неизвестна; числа остаются прежними. */
void stream_buffer_telemetry_down(struct stream_buffer *b, uint8_t sub, struct stream_address peer) {
    if (!b || sub >= b->descriptor.n || !b->telemetry[sub]) return;
    struct stream_telemetry *t = b->telemetry[sub];
    if (!stream_address_equal(t->rows[t->count - 1].node, peer)) return;
    for (unsigned i = 0; i < t->count; i++) t->rows[i].flags |= STREAM_TELEMETRY_STALE;
    t->rows[t->count - 1].flags |= STREAM_TELEMETRY_DOWN;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream telemetry DOWN sub=%u peer=%s", sub, STREAM_ADDRESS_TEXT(peer));
}
