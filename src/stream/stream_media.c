#include <string.h>

#include "../../lib/debug_config.h"
#include "stream_media.h"


/* Проверить весь набор описаний до чтения payload или выделения памяти под кадр. */
static int media_validate(const struct stream_media_block *b, size_t *video_size, size_t *audio_size, uint32_t *first) {
    if (!b || b->video_count > STREAM_MEDIA_MAX_ENTRIES || b->audio_count > STREAM_MEDIA_MAX_ENTRIES
        || !b->format.revision || (!b->video_count && !b->audio_count)
        || b->format.video_codec < STREAM_MEDIA_H264 || b->format.video_codec > STREAM_MEDIA_H266
        || b->format.audio_codec > STREAM_MEDIA_OPUS_STEREO || !b->format.width || b->format.width > 4096
        || !b->format.height || b->format.height > 4096 || b->format.rotation > 270 || b->format.rotation % 90) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media format/count invalid present=%d video=%u audio=%u codecs=%u/%u dimensions=%ux%u rotation=%u",
                    b != NULL, b ? b->video_count : 0, b ? b->audio_count : 0, b ? b->format.video_codec : 0,
                    b ? b->format.audio_codec : 0, b ? b->format.width : 0, b ? b->format.height : 0, b ? b->format.rotation : 0);
        return STREAM_ERR_FORMAT;
    }
    *video_size = *audio_size = 0; *first = UINT32_MAX;
    for (unsigned i = 0; i < b->video_count; i++) {
        const struct stream_media_video *v = &b->video[i];
        if (!v->data || !v->frame_id || !v->frame_size || v->frame_size > STREAM_MEDIA_MAX_FRAME || !v->length
            || v->offset >= v->frame_size || v->length > v->frame_size - v->offset || (v->flags & ~STREAM_MEDIA_KEY)
            || v->pts_us > INT64_MAX || v->length > STREAM_MEDIA_MAX_BLOCK - *video_size) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media video invalid entry=%u id=%llu offset=%u length=%u frame=%u flags=%u",
                        i, (unsigned long long)v->frame_id, v->offset, v->length, v->frame_size, v->flags);
            return STREAM_ERR_FORMAT;
        }
        if (i) {
            const struct stream_media_video *p = &b->video[i - 1];
            int same = p->frame_id == v->frame_id;
            if ((same && (v->offset != p->offset + p->length || v->frame_size != p->frame_size
                          || v->pts_us != p->pts_us || v->flags != p->flags))
                || (!same && (v->frame_id <= p->frame_id || v->offset || p->offset + p->length != p->frame_size
                              || v->pts_us <= p->pts_us))) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media video order invalid entry=%u frame=%llu/%llu offset=%u/%u pts=%llu/%llu",
                            i, (unsigned long long)p->frame_id, (unsigned long long)v->frame_id, p->offset + p->length, v->offset,
                            (unsigned long long)p->pts_us, (unsigned long long)v->pts_us);
                return STREAM_ERR_FORMAT;
            }
        }
        if (!v->offset && *first == UINT32_MAX) *first = (uint32_t)*video_size;
        *video_size += v->length;
    }
    for (unsigned i = 0; i < b->audio_count; i++) {
        const struct stream_media_audio *a = &b->audio[i];
        if (!b->format.audio_codec || !a->data || !a->length || a->length > STREAM_MEDIA_MAX_OPUS
            || a->samples != 960 || a->pts_us > INT64_MAX || (i && a->pts_us <= b->audio[i - 1].pts_us)) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media audio invalid entry=%u length=%u samples=%u", i, a->length, a->samples);
            return STREAM_ERR_FORMAT;
        }
        *audio_size += a->length;
    }
    return STREAM_OK;
}

/* Канонический header и последовательные payload-области исключают перекрытия фрагментов. */
int stream_media_encode(const struct stream_media_block *b, uint8_t *output, size_t capacity, size_t *length) {
    size_t video_size, audio_size; uint32_t first;
    if (!output || !length) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media encode missing output"); return STREAM_ERR_ARGUMENT; }
    int result = media_validate(b, &video_size, &audio_size, &first);
    if (result != STREAM_OK) return result;
    size_t header = STREAM_MEDIA_HEADER_SIZE + b->video_count * STREAM_MEDIA_VIDEO_SIZE + b->audio_count * STREAM_MEDIA_AUDIO_SIZE;
    size_t total = header + video_size + audio_size;
    if (total > capacity || total > STREAM_MEDIA_MAX_BLOCK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media encode capacity=%zu needed=%zu", capacity, total); return STREAM_ERR_ARGUMENT;
    }
    memset(output, 0, header);
    memcpy(output, "uAV2", 4);
    output[4] = b->format.video_codec; output[5] = b->format.audio_codec;
    output[6] = (uint8_t)b->video_count; output[7] = (uint8_t)b->audio_count;
    stream_write_be(output + 8, b->format.width, 2); stream_write_be(output + 10, b->format.height, 2);
    stream_write_be(output + 12, b->format.rotation, 2); stream_write_be(output + 14, header, 2);
    stream_write_be(output + 16, video_size, 4); stream_write_be(output + 20, audio_size, 4);
    stream_write_be(output + 24, first, 4); stream_write_be(output + 28, b->format.revision, 4);
    size_t payload = header, entry = STREAM_MEDIA_HEADER_SIZE;
    for (unsigned i = 0; i < b->video_count; i++, entry += STREAM_MEDIA_VIDEO_SIZE) {
        const struct stream_media_video *v = &b->video[i];
        stream_write_be(output + entry, v->frame_id, 8); stream_write_be(output + entry + 8, v->pts_us, 8);
        stream_write_be(output + entry + 16, v->frame_size, 4); stream_write_be(output + entry + 20, v->offset, 4);
        stream_write_be(output + entry + 24, v->length, 4); output[entry + 28] = v->flags;
        memcpy(output + payload, v->data, v->length); payload += v->length;
    }
    for (unsigned i = 0; i < b->audio_count; i++, entry += STREAM_MEDIA_AUDIO_SIZE) {
        const struct stream_media_audio *a = &b->audio[i];
        stream_write_be(output + entry, a->pts_us, 8); stream_write_be(output + entry + 8, a->length, 4);
        stream_write_be(output + entry + 12, a->samples, 2);
        memcpy(output + payload, a->data, a->length); payload += a->length;
    }
    *length = total;
    return STREAM_OK;
}

/* Проверить арифметику областей до вычисления указателей, затем валидировать все описания. */
int stream_media_parse(const uint8_t *data, size_t length, struct stream_media_block *out) {
    if (!data || !out || length < STREAM_MEDIA_HEADER_SIZE || length > STREAM_MEDIA_MAX_BLOCK || memcmp(data, "uAV2", 4)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media parse bad prefix/length=%zu", length); return STREAM_ERR_FORMAT;
    }
    struct stream_media_block b = {.video_count = data[6], .audio_count = data[7]};
    b.format = (struct stream_media_format){(uint16_t)stream_read_be(data + 8, 2), (uint16_t)stream_read_be(data + 10, 2),
        (uint16_t)stream_read_be(data + 12, 2), data[4], data[5], (uint32_t)stream_read_be(data + 28, 4)};
    size_t header = STREAM_MEDIA_HEADER_SIZE + b.video_count * STREAM_MEDIA_VIDEO_SIZE + b.audio_count * STREAM_MEDIA_AUDIO_SIZE;
    size_t video_size = (size_t)stream_read_be(data + 16, 4), audio_size = (size_t)stream_read_be(data + 20, 4);
    if (b.video_count > STREAM_MEDIA_MAX_ENTRIES || b.audio_count > STREAM_MEDIA_MAX_ENTRIES || header > length
        || stream_read_be(data + 14, 2) != header || video_size > length - header || audio_size != length - header - video_size
        || !b.format.revision) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media parse header/regions invalid length=%zu header=%zu video=%zu audio=%zu",
                    length, header, video_size, audio_size); return STREAM_ERR_FORMAT;
    }
    size_t entry = STREAM_MEDIA_HEADER_SIZE, payload = header;
    for (unsigned i = 0; i < b.video_count; i++, entry += STREAM_MEDIA_VIDEO_SIZE) {
        struct stream_media_video *v = &b.video[i];
        *v = (struct stream_media_video){.frame_id = stream_read_be(data + entry, 8), .pts_us = stream_read_be(data + entry + 8, 8),
            .frame_size = (uint32_t)stream_read_be(data + entry + 16, 4), .offset = (uint32_t)stream_read_be(data + entry + 20, 4),
            .length = (uint32_t)stream_read_be(data + entry + 24, 4), .flags = data[entry + 28], .data = data + payload};
        if (v->length > header + video_size - payload || data[entry + 29] || data[entry + 30] || data[entry + 31]) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media parse video bounds/reserved entry=%u", i); return STREAM_ERR_FORMAT;
        }
        payload += v->length;
    }
    if (payload != header + video_size) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media parse video size mismatch"); return STREAM_ERR_FORMAT;
    }
    for (unsigned i = 0; i < b.audio_count; i++, entry += STREAM_MEDIA_AUDIO_SIZE) {
        struct stream_media_audio *a = &b.audio[i];
        *a = (struct stream_media_audio){.pts_us = stream_read_be(data + entry, 8),
            .length = (uint32_t)stream_read_be(data + entry + 8, 4), .samples = (uint16_t)stream_read_be(data + entry + 12, 2),
            .data = data + payload};
        if (a->length > length - payload || data[entry + 14] || data[entry + 15]) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media parse audio bounds/reserved entry=%u", i); return STREAM_ERR_FORMAT;
        }
        payload += a->length;
    }
    size_t actual_video, actual_audio; uint32_t first;
    int result = media_validate(&b, &actual_video, &actual_audio, &first);
    if (result != STREAM_OK) return result;
    if (actual_video != video_size || actual_audio != audio_size || stream_read_be(data + 24, 4) != first) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media parse payload/first offset mismatch"); return STREAM_ERR_FORMAT;
    }
    b.first_frame_offset = first; *out = b;
    return STREAM_OK;
}

int stream_media_key_parse(const uint8_t *data, size_t length, const uint8_t **config, size_t *config_length,
                           const uint8_t **frame, size_t *frame_length) {
    if (!data || length < 5 || length > STREAM_MEDIA_MAX_FRAME || !config || !config_length || !frame || !frame_length) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "KEY envelope invalid length=%zu", length); return STREAM_ERR_FORMAT;
    }
    size_t n = (size_t)stream_read_be(data, 4);
    if (n > STREAM_MEDIA_MAX_CONFIG || n >= length - 4) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "KEY configuration invalid config=%zu total=%zu", n, length); return STREAM_ERR_FORMAT;
    }
    *config = data + 4; *config_length = n; *frame = data + 4 + n; *frame_length = length - 4 - n;
    return STREAM_OK;
}

