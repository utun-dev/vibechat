#include <limits.h>
#include <string.h>

#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "stream_media_buffer.h"

struct media_position {
    uint64_t first, last, key_seq;
    int64_t video_us, audio_us, key_us;
    uint32_t generation;
    int gap;
};
struct stream_media_store {
    struct stream_buffer *buffer;
    struct stream_descriptor descriptor;
    struct media_position *positions;
    unsigned capacity, head, count;
    uint32_t generation;
    int configured, waiting_key, finished, failed;
    struct stream_media_format format;
    struct stream_media_video pending;
    uint64_t pending_seq, key_seq, key_last_seq, last_frame_id;
    uint32_t pending_used;
    int64_t video_us, audio_us, key_us, last_audio_us;
    uint64_t pressure_report_us, pending_started_us, key_completed_us;
};
struct cursor_track { uint64_t seq; unsigned entry; int ended; };
struct stream_media_cursor {
    struct stream_media_store *store;
    struct cursor_track video, audio;
    uint32_t generation, video_revision;
    int initialized;
    unsigned tracks;
    int64_t start_us;
    int audio_started;
    int64_t audio_start_us;
    uint64_t created_us;
    uint8_t *scratch[2], *frame;
    size_t scratch_capacity, frame_capacity;
    uint64_t scratch_seq[2];
    size_t scratch_length[2];
    int scratch_valid[2];
    unsigned scratch_next;
    uint8_t opus[STREAM_MEDIA_MAX_OPUS];
    struct stream_media_video pending;
    struct stream_media_format pending_format;
    uint32_t pending_used;
};

/* Логический порядок сохранённых диапазонов не зависит от переиспользования массива. */
static struct media_position *media_at(struct stream_media_store *s, unsigned index) {
    return &s->positions[(s->head + index) % s->capacity];
}

/* Диапазоны DATA/GAP идут по seq; бинарный поиск не сканирует всё окно на каждом аудиопакете. */
static struct media_position *media_find(struct stream_media_store *s, uint64_t seq) {
    unsigned low = 0, high = s->count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        struct media_position *p = media_at(s, middle);
        if (p->last < seq) low = middle + 1; else high = middle;
    }
    if (low == s->count) return NULL;
    struct media_position *p = media_at(s, low);
    return p->first <= seq ? p : NULL;
}

/* Индекс не удерживает старые части; его предел памяти также не зависит от продолжительности просмотра. */
void stream_media_store_trim(struct stream_media_store *s) {
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media store trim: null store"); return; }
    struct stream_buffer_stats stats;
    if (stream_buffer_get_stats(s->buffer, &stats) != STREAM_OK) return;
    while (s->count && media_at(s, 0)->last < stats.expired_before) {
        s->head = (s->head + 1) % s->capacity; s->count--;
    }
}

/* Один элемент на подтверждённую пачку/пропуск, без копирования H264/Opus. */
static void media_append(struct stream_media_store *s, struct media_position position) {
    stream_media_store_trim(s);
    if (s->count == s->capacity) {
        uint64_t now = get_time_us();
        if (!s->pressure_report_us || now - s->pressure_report_us >= 1000000) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media index pressure source=%s oldest=%llu incoming=%llu capacity=%u",
                       STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)media_at(s, 0)->first,
                       (unsigned long long)position.first, s->capacity);
            s->pressure_report_us = now;
        }
        s->head = (s->head + 1) % s->capacity; s->count--;
    }
    *media_at(s, s->count++) = position;
}

/* Геометрия индекса ограничена вместимостью RS и отдельным лимитом метаданных. */
int stream_media_store_create(struct stream_media_store **out, struct stream_buffer *buffer, const struct stream_descriptor *d) {
    if (!out || *out || !buffer || stream_descriptor_validate(d) != STREAM_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media store create invalid arguments"); return STREAM_ERR_ARGUMENT;
    }
    struct stream_buffer_stats stats;
    if (stream_buffer_get_stats(buffer, &stats) != STREAM_OK) return STREAM_ERR_ARGUMENT;
    struct stream_media_store *s = u_calloc(1, sizeof(*s));
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media store allocation failed"); return STREAM_ERR_MEMORY; }
    size_t slots = stats.capacity_bytes / stream_block_capacity(d);
    s->capacity = (unsigned)(2 * (slots / (d->n - d->r)) + 1);
    unsigned maximum = STREAM_MEDIA_INDEX_BYTES / sizeof(*s->positions);
    if (s->capacity > maximum) s->capacity = maximum;
    s->positions = u_calloc(s->capacity, sizeof(*s->positions));
    if (!s->positions) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media index allocation failed slots=%u", s->capacity); u_free(s); return STREAM_ERR_MEMORY;
    }
    s->buffer = buffer; s->descriptor = *d; s->generation = 1; s->waiting_key = 1;
    s->video_us = s->audio_us = s->key_us = s->last_audio_us = -1; s->key_seq = s->key_last_seq = UINT64_MAX;
    *out = s;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "media index created source=%s records=%u bytes=%zu shared_RS=1",
               STREAM_ADDRESS_TEXT(d->source), s->capacity, s->capacity * sizeof(*s->positions));
    return STREAM_OK;
}

/* Store освобождается после прекращения обращений reader; cursor destroy не разыменовывает store. */
void stream_media_store_destroy(struct stream_media_store *s) {
    if (!s) return;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "media index destroy source=%s records=%u generation=%u",
               STREAM_ADDRESS_TEXT(s->descriptor.source), s->count, s->generation);
    u_free(s->positions); u_free(s);
}

/* Новая цепочка начинается с KEY; старые reader увидят смену generation до выдачи новых данных. */
static int media_discontinuity(struct stream_media_store *s) {
    if (s->generation == UINT32_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media generation exhausted source=%s", STREAM_ADDRESS_TEXT(s->descriptor.source));
        s->failed = 1; return STREAM_ERR_FORMAT;
    }
    s->generation++; s->pending_used = 0; s->waiting_key = 1;
    s->video_us = s->audio_us = s->key_us = s->last_audio_us = -1; s->key_seq = s->key_last_seq = UINT64_MAX;
    return STREAM_OK;
}

/* Окончательный пропуск завершает старый непрерывный участок; следующая цепочка начинается с KEY. */
void stream_media_store_gap(struct stream_media_store *s, uint64_t first, uint64_t last) {
    if (!s || first > last || last == UINT64_MAX || s->generation == UINT32_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media index gap invalid first=%llu last=%llu", (unsigned long long)first, (unsigned long long)last);
        if (s) s->failed = 1;
        return;
    }
    media_append(s, (struct media_position){.first = first, .last = last, .generation = s->generation, .gap = 1,
        .video_us = s->video_us, .audio_us = s->audio_us, .key_seq = s->key_seq, .key_us = s->key_us});
    media_discontinuity(s);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "media index GAP source=%s first=%llu last=%llu generation=%u",
               STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)first, (unsigned long long)last, s->generation);
}

/* Метаданные полного кадра считаются готовыми только после всех последовательных подписанных фрагментов. */
int stream_media_store_feed(struct stream_media_store *s, uint64_t seq, const uint8_t *data, size_t length) {
    if (!s || !data || seq == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media index feed invalid seq=%llu", (unsigned long long)seq); return STREAM_ERR_ARGUMENT;
    }
    if (s->failed) return STREAM_ERR_FORMAT;
    if (s->count && seq <= media_at(s, s->count - 1)->last) return STREAM_DUPLICATE;
    struct stream_media_block b;
    int result = stream_media_parse(data, length, &b);
    if (result != STREAM_OK) { s->failed = 1; return result; }
    if (s->configured && (b.format.audio_codec != s->format.audio_codec || b.format.revision < s->format.revision
        || (b.format.revision == s->format.revision && (b.format.video_codec != s->format.video_codec || b.format.width != s->format.width
            || b.format.height != s->format.height || b.format.rotation != s->format.rotation)))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media index conflicting format source=%s revision=%u/%u audio=%u/%u",
                    STREAM_ADDRESS_TEXT(s->descriptor.source), s->format.revision, b.format.revision,
                    s->format.audio_codec, b.format.audio_codec); s->failed = 1; return STREAM_ERR_CONFLICT;
    }
    if (s->configured && b.format.revision != s->format.revision) { s->pending_used = 0; s->waiting_key = 1; }
    s->format = b.format; s->configured = 1;
    int64_t audio_previous = s->last_audio_us;
    for (unsigned i = 0; i < b.audio_count; i++) {
        int64_t pts = (int64_t)b.audio[i].pts_us;
        if (pts > INT64_MAX - 20000) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media audio end overflow seq=%llu pts=%lld", (unsigned long long)seq, (long long)pts);
            s->failed = 1; return STREAM_ERR_FORMAT;
        }
        if (audio_previous >= 0 && pts - audio_previous > 120000) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media audio discontinuity seq=%llu previous=%lld pts=%lld; require new KEY",
                       (unsigned long long)seq, (long long)audio_previous, (long long)pts);
            if (media_discontinuity(s) != STREAM_OK) return STREAM_ERR_FORMAT;
        }
        if (pts > audio_previous) audio_previous = pts;
    }
    for (unsigned i = 0; i < b.video_count; i++) {
        const struct stream_media_video *v = &b.video[i];
        if (s->waiting_key && !s->pending_used && v->offset) continue;
        if (!v->offset) {
            if (s->video_us >= 0 && ((int64_t)v->pts_us <= s->video_us || v->frame_id <= s->last_frame_id)) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media index stale video seq=%llu frame=%llu pts=%llu",
                           (unsigned long long)seq, (unsigned long long)v->frame_id, (unsigned long long)v->pts_us);
                continue;
            }
            if (s->pending_used) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media index incomplete frame=%llu before frame=%llu; require KEY",
                           (unsigned long long)s->pending.frame_id, (unsigned long long)v->frame_id);
                if (media_discontinuity(s) != STREAM_OK) return STREAM_ERR_FORMAT;
            }
            if (s->waiting_key && !(v->flags & STREAM_MEDIA_KEY)) continue;
            s->pending = *v; s->pending.data = NULL; s->pending_seq = seq; s->pending_started_us = get_time_us();
        }
        if (s->waiting_key && !(v->flags & STREAM_MEDIA_KEY)) continue;
        if (v->frame_id != s->pending.frame_id || v->offset != s->pending_used || v->pts_us != s->pending.pts_us
            || v->frame_size != s->pending.frame_size || v->flags != s->pending.flags) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media index frame discontinuity seq=%llu frame=%llu offset=%u expected=%u",
                       (unsigned long long)seq, (unsigned long long)v->frame_id, v->offset, s->pending_used);
            if (media_discontinuity(s) != STREAM_OK) return STREAM_ERR_FORMAT;
            continue;
        }
        s->pending_used += v->length;
        if (s->pending_used != s->pending.frame_size) continue;
        s->video_us = (int64_t)v->pts_us; s->last_frame_id = v->frame_id; s->waiting_key = 0; s->pending_used = 0;
        if (v->flags & STREAM_MEDIA_KEY) {
            s->key_seq = s->pending_seq; s->key_last_seq = seq;
            s->key_us = (int64_t)v->pts_us; s->key_completed_us = get_time_us();
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
                "media KEY indexed source=%s epoch=%llx pts=%lld seq=%llu..%llu bytes=%u assemble_us=%llu",
                STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)s->descriptor.source_epoch,
                (long long)s->key_us, (unsigned long long)s->key_seq, (unsigned long long)seq, v->frame_size,
                (unsigned long long)(s->key_completed_us - s->pending_started_us));
        }
    }
    for (unsigned i = 0; i < b.audio_count; i++) {
        const struct stream_media_audio *a = &b.audio[i];
        if (s->last_audio_us >= 0 && (int64_t)a->pts_us <= s->last_audio_us) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media index stale audio seq=%llu pts=%llu", (unsigned long long)seq, (unsigned long long)a->pts_us);
            continue;
        }
        s->last_audio_us = (int64_t)a->pts_us; s->audio_us = s->last_audio_us + 20000;
    }
    media_append(s, (struct media_position){.first = seq, .last = seq, .generation = s->generation,
        .video_us = s->video_us, .audio_us = s->audio_us, .key_seq = s->key_seq, .key_us = s->key_us});
    return STREAM_OK;
}

void stream_media_store_finish(struct stream_media_store *s) {
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media index finish: null store"); return; }
    s->finished = 1;
}

/* Индекс уже нашёл последний полный KEY при сборке: незавершённый следующий кадр его не заменяет. */
int stream_media_store_recovery(struct stream_media_store *s, struct stream_media_recovery *out) {
    if (!s || !out || s->failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media recovery snapshot unavailable"); return STREAM_ERR_STATE;
    }
    stream_media_store_trim(s);
    struct stream_media_recovery position = {.last_seq = UINT64_MAX, .key_seq = UINT64_MAX, .key_last_seq = UINT64_MAX,
        .video_us = s->video_us, .key_us = -1};
    if (s->count) position.last_seq = media_at(s, s->count - 1)->last;
    if (s->key_seq != UINT64_MAX && media_find(s, s->key_seq) && media_find(s, s->key_last_seq)) {
        position.key_seq = s->key_seq; position.key_last_seq = s->key_last_seq; position.key_valid = 1; position.key_us = s->key_us;
    }
    *out = position; return STREAM_OK;
}

/* Курсор хранит только собственные рабочие буферы, не копию накопленного медиапотока. */
int stream_media_cursor_create(struct stream_media_cursor **out, struct stream_media_store *s, unsigned tracks) {
    if (!out || *out || !s || !(tracks & STREAM_MEDIA_TRACK_VIDEO) || (tracks & ~3u)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media cursor create invalid arguments tracks=%u", tracks); return STREAM_ERR_ARGUMENT;
    }
    struct stream_media_cursor *c = u_calloc(1, sizeof(*c));
    if (!c) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media cursor allocation failed"); return STREAM_ERR_MEMORY; }
    c->store = s; c->tracks = tracks; c->scratch_capacity = stream_input_size(&s->descriptor); c->created_us = get_time_us();
    c->audio.ended = !(tracks & STREAM_MEDIA_TRACK_AUDIO);
    if (c->scratch_capacity > STREAM_MEDIA_MAX_BLOCK) c->scratch_capacity = STREAM_MEDIA_MAX_BLOCK;
    for (unsigned i = 0; i < 2; i++) c->scratch[i] = u_malloc((uint32_t)c->scratch_capacity);
    if (!c->scratch[0] || !c->scratch[1]) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media cursor scratch allocation failed bytes=%zu", c->scratch_capacity);
        stream_media_cursor_destroy(c); return STREAM_ERR_MEMORY;
    }
    *out = c;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "media cursor created source=%s scratch=%zu", STREAM_ADDRESS_TEXT(s->descriptor.source),
               2 * c->scratch_capacity);
    return STREAM_OK;
}

void stream_media_cursor_destroy(struct stream_media_cursor *c) {
    if (!c) return;
    u_free(c->scratch[0]); u_free(c->scratch[1]); u_free(c->frame); u_free(c);
}

void stream_media_cursor_reset(struct stream_media_cursor *c) {
    if (!c) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media cursor reset: null cursor"); return; }
    c->initialized = 0; c->video_revision = 0; c->pending_used = 0;
    c->audio_started = 0; c->audio_start_us = -1;
    c->pending = (struct stream_media_video){0}; c->pending_format = (struct stream_media_format){0};
    c->scratch_valid[0] = c->scratch_valid[1] = 0;
    c->video = (struct cursor_track){0}; c->audio = (struct cursor_track){0};
    c->audio.ended = !(c->tracks & STREAM_MEDIA_TRACK_AUDIO); c->created_us = get_time_us();
}

/* Выбирается последний полный KEY; тёплая история аудио перед ним остаётся в том же RS-кольце. */
static int cursor_start(struct stream_media_cursor *c) {
    struct stream_media_store *s = c->store;
    stream_media_store_trim(s);
    if (s->failed) return STREAM_ERR_FORMAT;
    if (c->initialized) return STREAM_OK;
    if (!s->count || s->key_seq == UINT64_MAX || !media_find(s, s->key_seq)) return s->finished ? STREAM_GAP : STREAM_WAIT;
    c->generation = s->generation; c->start_us = s->key_us; c->audio_start_us = -1;
    c->video.seq = s->key_seq; c->audio.seq = s->key_seq;
    for (unsigned i = 0; i < s->count; i++) {
        struct media_position *p = media_at(s, i);
        if (p->generation == c->generation && !p->gap && p->audio_us > c->start_us - STREAM_MEDIA_PREROLL_US) {
            c->audio.seq = p->first; break;
        }
    }
    c->initialized = 1;
    uint64_t now = get_time_us();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM,
               "media cursor start source=%s epoch=%llx key_seq=%llu pts=%lld audio_seq=%llu generation=%u "
               "wait_key_us=%llu key_cached_us=%llu video_ready_us=%lld audio_ready_us=%lld key_backlog_us=%lld",
               STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)s->descriptor.source_epoch,
               (unsigned long long)c->video.seq, (long long)c->start_us,
               (unsigned long long)c->audio.seq, c->generation, (unsigned long long)(now - c->created_us),
               (unsigned long long)(s->key_completed_us ? now - s->key_completed_us : 0),
               (long long)s->video_us, (long long)s->audio_us, (long long)(s->video_us - c->start_us));
    return STREAM_OK;
}

/* Граница относится к участку этого reader, а не к данным после ещё не пройденного GAP. */
int stream_media_cursor_status(struct stream_media_cursor *c, struct stream_media_status *out) {
    if (!c || !out) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media cursor status invalid arguments"); return STREAM_ERR_ARGUMENT; }
    struct stream_media_store *s = c->store;
    int result = cursor_start(c);
    struct stream_media_status status = {.ready_us = -1, .video_us = -1, .audio_us = -1, .start_us = -1,
        .audio_enabled = s->configured && s->format.audio_codec && (c->tracks & STREAM_MEDIA_TRACK_AUDIO), .finished = s->finished};
    if (result < 0) return result;
    if (c->initialized) {
        for (unsigned i = s->count; i-- > 0;) {
            struct media_position *p = media_at(s, i);
            if (p->generation != c->generation) continue;
            status.video_us = p->video_us; status.audio_us = p->audio_us; break;
        }
        status.ready_us = status.video_us;
        if (status.audio_enabled && status.audio_us < status.ready_us) status.ready_us = status.audio_us;
        if (s->finished && c->video.ended && status.audio_enabled) status.ready_us = status.audio_us;
        if (s->finished && c->audio.ended && status.video_us > status.ready_us) status.ready_us = status.video_us;
        status.video_ended = c->video.ended; status.audio_ended = c->audio.ended;
        status.start_us = c->start_us; status.generation = c->generation; status.initialized = 1;
    }
    *out = status;
    return STREAM_OK;
}

/* Два scratch сохраняют соседние audio/video пачки; память кольца никогда не покидает uasync. */
static int cursor_block(struct stream_media_cursor *c, uint64_t seq, struct stream_media_block *block) {
    struct stream_media_store *s = c->store;
    if (s->count && seq < media_at(s, 0)->first) return STREAM_EXPIRED;
    struct media_position *p = media_find(s, seq);
    if (!p) return s->finished ? STREAM_GAP : STREAM_WAIT;
    if (p->gap || p->generation != c->generation) return STREAM_GAP;
    unsigned slot = 0;
    for (; slot < 2; slot++) if (c->scratch_valid[slot] && c->scratch_seq[slot] == seq) break;
    size_t length = 0;
    if (slot == 2) {
        slot = c->scratch_next++ % 2;
        int result = stream_buffer_decode(s->buffer, seq, c->scratch[slot], c->scratch_capacity, &length);
        if (result != STREAM_OK) return result == STREAM_REJECTED ? STREAM_GAP : result;
        c->scratch_seq[slot] = seq; c->scratch_length[slot] = length; c->scratch_valid[slot] = 1;
    } else {
        length = c->scratch_length[slot];
    }
    return stream_media_parse(c->scratch[slot], length, block);
}

/* Позиции треков независимы: чтение видео не вытаскивает дальний звук из общего кольца. */
static int cursor_peek(struct stream_media_cursor *c, struct cursor_track *track, int video,
                        struct stream_media_block *block, int64_t *pts) {
    if (track->ended) return STREAM_GAP;
    for (unsigned work = 0; work < 8; work++) {
        int result = cursor_block(c, track->seq, block);
        if (result != STREAM_OK) {
            struct stream_media_store *s = c->store;
            if (s->finished && s->count && track->seq > media_at(s, s->count - 1)->last) { track->ended = 1; return STREAM_GAP; }
            return result;
        }
        unsigned count = video ? block->video_count : block->audio_count;
        if (track->entry < count) {
            *pts = (int64_t)(video ? block->video[track->entry].pts_us : block->audio[track->entry].pts_us);
            return STREAM_OK;
        }
        track->seq++; track->entry = 0;
    }
    return STREAM_MORE;
}

/* Сборка одного следующего видеокадра; остальные кадры продолжают храниться только в RS. */
static int cursor_video(struct stream_media_cursor *c, struct stream_media_packet *packet) {
    for (unsigned work = 0; work < 16; work++) {
        struct stream_media_block b; int64_t pts;
        int result = cursor_peek(c, &c->video, 1, &b, &pts);
        if (result != STREAM_OK) return result;
        const struct stream_media_video *v = &b.video[c->video.entry++];
        if (b.format.revision != c->pending_format.revision) {
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "media cursor revision source=%s seq=%llu revision=%u->%u pending=%u; require KEY",
                STREAM_ADDRESS_TEXT(c->store->descriptor.source), (unsigned long long)c->video.seq,
                c->pending_format.revision, b.format.revision, c->pending_used);
            c->pending = (struct stream_media_video){0}; c->pending_used = 0; c->pending_format = b.format;
        }
        /* Revision относится к уже выданному видео этого reader, а не к текущему live store. */
        if (b.format.revision != c->video_revision && !(v->flags & STREAM_MEDIA_KEY)) continue;
        if (!c->pending.frame_size && v->offset) continue;
        if (!c->video_revision && !c->pending.frame_size
            && (v->offset || !(v->flags & STREAM_MEDIA_KEY) || pts != c->start_us)) continue;
        if (!v->offset) {
            if (!c->video_revision && (!(v->flags & STREAM_MEDIA_KEY) || pts != c->start_us)) continue;
            if (v->frame_size > c->frame_capacity) {
                uint8_t *frame = u_realloc(c->frame, v->frame_size);
                if (!frame) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media cursor frame allocation failed bytes=%u", v->frame_size); return STREAM_ERR_MEMORY; }
                c->frame = frame; c->frame_capacity = v->frame_size;
            }
            c->pending = *v; c->pending.data = NULL; c->pending_used = 0; c->pending_format = b.format;
        }
        if (!c->pending.frame_size || v->frame_id != c->pending.frame_id || v->offset != c->pending_used
            || v->pts_us != c->pending.pts_us || v->frame_size != c->pending.frame_size || v->flags != c->pending.flags) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media cursor fragment conflict seq=%llu frame=%llu offset=%u used=%u",
                       (unsigned long long)c->video.seq, (unsigned long long)v->frame_id, v->offset, c->pending_used);
            return STREAM_GAP;
        }
        memcpy(c->frame + c->pending_used, v->data, v->length); c->pending_used += v->length;
        if (c->pending_used != c->pending.frame_size) continue;
        struct stream_media_packet out = {.kind = STREAM_MEDIA_READ_VIDEO, .format = c->pending_format, .video = c->pending};
        out.video.offset = 0; out.video.length = out.video.frame_size; out.video.data = c->frame;
        if (c->video_revision != b.format.revision) DEBUG_INFO(DEBUG_CATEGORY_STREAM,
            "media cursor KEY ready source=%s revision=%u frame=%llu pts=%llu bytes=%u",
            STREAM_ADDRESS_TEXT(c->store->descriptor.source), b.format.revision,
            (unsigned long long)v->frame_id, (unsigned long long)v->pts_us, v->frame_size);
        c->pending_used = 0; c->pending.frame_size = 0; c->video_revision = b.format.revision; *packet = out;
        return STREAM_OK;
    }
    return STREAM_MORE;
}

/* Видео выдаётся по готовности decoder, звук — по времени потребителя. */
int stream_media_cursor_read(struct stream_media_cursor *c, int64_t position, int want_video, int64_t audio_ahead,
                             struct stream_media_packet *packet) {
    if (!c || !packet || (want_video != 0 && want_video != 1) || audio_ahead < -1 || audio_ahead > 60000000) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media cursor read invalid arguments video=%d audio_ahead=%lld",
                    want_video, (long long)audio_ahead); return STREAM_ERR_ARGUMENT;
    }
    int result = cursor_start(c);
    if (result != STREAM_OK) {
        if (result == STREAM_GAP && c->store->finished && !c->initialized) {
            *packet = (struct stream_media_packet){.kind = STREAM_MEDIA_READ_END}; return STREAM_OK;
        }
        return result;
    }
    struct stream_media_block audio, video; int64_t audio_pts = INT64_MAX, video_pts = INT64_MAX;
    if (!c->store->format.audio_codec) c->audio.ended = 1;
    int ar = c->audio.ended ? STREAM_GAP : audio_ahead < 0 ? STREAM_WAIT : cursor_peek(c, &c->audio, 0, &audio, &audio_pts);
    int vr = want_video ? cursor_peek(c, &c->video, 1, &video, &video_pts) : STREAM_WAIT;
    if ((ar == STREAM_GAP && !c->audio.ended) || (vr == STREAM_GAP && !c->video.ended)) return STREAM_GAP;
    if (ar == STREAM_EXPIRED || vr == STREAM_EXPIRED) return STREAM_EXPIRED;
    if (ar < 0 || vr < 0) return ar < 0 ? ar : vr;
    if (c->audio.ended && c->video.ended) {
        *packet = (struct stream_media_packet){.kind = STREAM_MEDIA_READ_END}; return STREAM_OK;
    }
    /* Поздний первый звук должен прогреть decoder до запуска audio clock. Основание фиксировано,
     * поэтому неподвижные часы разрешают только один lookahead, а не всё оставшееся аудио. */
    if (ar == STREAM_OK && c->audio_start_us < 0) {
        c->audio_start_us = audio_pts > c->start_us ? audio_pts : c->start_us;
        if (audio_pts > c->start_us) DEBUG_INFO(DEBUG_CATEGORY_STREAM,
            "media cursor audio startup source=%s video_pts=%lld audio_pts=%lld gap_us=%lld",
            STREAM_ADDRESS_TEXT(c->store->descriptor.source), (long long)c->start_us,
            (long long)audio_pts, (long long)(audio_pts - c->start_us));
    }
    int64_t audio_position = position > c->audio_start_us ? position : c->audio_start_us;
    int64_t audio_limit = audio_ahead < 0 ? -1 : position < 0 ? c->start_us
        : audio_position > INT64_MAX - audio_ahead ? INT64_MAX : audio_position + audio_ahead;
    int video_allowed = want_video && vr == STREAM_OK;
    if (ar == STREAM_OK && audio_ahead >= 0 && audio_pts <= audio_limit && (audio_pts < video_pts || !video_allowed)) {
        /* video peek мог переиспользовать scratch аудио: заново взять ту же пачку из двухэлементного cache. */
        result = cursor_block(c, c->audio.seq, &audio);
        if (result != STREAM_OK) return result;
        struct stream_media_audio a = audio.audio[c->audio.entry++];
        if (!c->audio_started) {
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "media cursor first audio source=%s seq=%llu pts=%llu start=%lld lead_us=%lld",
                STREAM_ADDRESS_TEXT(c->store->descriptor.source), (unsigned long long)c->audio.seq,
                (unsigned long long)a.pts_us, (long long)c->start_us, (long long)((int64_t)a.pts_us - c->start_us));
            c->audio_started = 1;
        }
        memcpy(c->opus, a.data, a.length); a.data = c->opus;
        *packet = (struct stream_media_packet){.kind = STREAM_MEDIA_READ_AUDIO, .format = audio.format, .audio = a};
        return STREAM_OK;
    }
    if (video_allowed) return cursor_video(c, packet);
    return ar == STREAM_MORE || vr == STREAM_MORE ? STREAM_MORE : STREAM_WAIT;
}
