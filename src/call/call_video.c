#include "call_video.h"
#include "call_video_rate.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <string.h>

static uint64_t read_be(const uint8_t *p, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++) value = (value << 8) | p[i];
    return value;
}
static void write_be(uint8_t *p, uint64_t value, unsigned count) {
    for (unsigned i = count; i; i--) { p[i - 1] = (uint8_t)value; value >>= 8; }
}
/* H.264 Annex A (Baseline), H.265 Annex A (Main tier). Picture/rate в MB для AVC,
 * в luma samples для HEVC; BR/CPB в кбит. Используем VCL-пределы без NAL-множителя.
 * Сверено: FFmpeg libavcodec/h264_levels.c, h265_profile_level.c; x265 encoder/level.cpp. */
struct video_level { uint8_t id; uint32_t picture, rate, kbps, cpb_kbits; };
static const struct video_level avc_levels[] = {
    {10, 99, 1485, 64, 175}, {11, 396, 3000, 192, 500}, {12, 396, 6000, 384, 1000},
    {13, 396, 11880, 768, 2000}, {20, 396, 11880, 2000, 2000}, {21, 792, 19800, 4000, 4000},
    {22, 1620, 20250, 4000, 4000}, {30, 1620, 40500, 10000, 10000}, {31, 3600, 108000, 14000, 14000},
    {32, 5120, 216000, 20000, 20000}, {40, 8192, 245760, 20000, 25000}, {41, 8192, 245760, 50000, 62500},
    {42, 8704, 522240, 50000, 62500}, {50, 22080, 589824, 135000, 135000}, {51, 36864, 983040, 240000, 240000},
    {52, 36864, 2073600, 240000, 240000}, {60, 139264, 4177920, 240000, 240000},
    {61, 139264, 8355840, 480000, 480000}, {62, 139264, 16711680, 800000, 800000}
};
static const struct video_level hevc_levels[] = {
    {30, 36864, 552960, 128, 350}, {60, 122880, 3686400, 1500, 1500}, {63, 245760, 7372800, 3000, 3000},
    {90, 552960, 16588800, 6000, 6000}, {93, 983040, 33177600, 10000, 10000},
    {120, 2228224, 66846720, 12000, 12000}, {123, 2228224, 133693440, 20000, 20000},
    {150, 8912896, 267386880, 25000, 25000}, {153, 8912896, 534773760, 40000, 40000},
    {156, 8912896, 1069547520, 60000, 60000}, {180, 35651584, 1069547520, 60000, 60000},
    {183, 35651584, 2139095040, 120000, 120000}, {186, 35651584, 4278190080u, 240000, 240000}
};
static const struct video_level *level_find(int codec, uint8_t id) {
    const struct video_level *levels = codec == CALL_VIDEO_H264 ? avc_levels : hevc_levels;
    unsigned count = codec == CALL_VIDEO_H264 ? sizeof(avc_levels) / sizeof(*levels) : sizeof(hevc_levels) / sizeof(*levels);
    if (codec != CALL_VIDEO_H264 && codec != CALL_VIDEO_H265) return NULL;
    for (unsigned i = 0; i < count; i++) if (levels[i].id == id) return &levels[i];
    return NULL;
}
static int limit_valid(const struct call_video_limit *l, int codec) {
    if (!l->width && !l->height && !l->fps && !l->pixels_per_second
        && !l->maximum_bps && !l->profiles && !l->level) return 1;
    return l->width >= 16 && l->width <= 4096 && l->height >= 16 && l->height <= 4096
        && l->fps >= 1 && l->fps <= 60 && l->pixels_per_second >= 256 && l->pixels_per_second <= 4096u * 4096u * 60u
        && l->maximum_bps >= CALL_VIDEO_RATE_MIN_BPS && l->maximum_bps <= CALL_VIDEO_RATE_MAX_BPS
        && l->profiles && !(l->profiles & ~3u) && level_find(codec, l->level)
        && !(codec == CALL_VIDEO_H265 && (l->profiles & CALL_VIDEO_MAIN_HIGH_TIER) && l->level < 120);
}
int call_video_caps_valid(const struct call_video_caps *caps) {
    if (!caps) return 0;
    for (unsigned i = 0; i < 2; i++) if (!limit_valid(&caps->encode[i], i + 1) || !limit_valid(&caps->decode[i], i + 1)) return 0;
    return 1;
}
int call_video_limit_orientations(struct call_video_limit *limit, int codec) {
    if (!limit || (codec != CALL_VIDEO_H264 && codec != CALL_VIDEO_H265) || !limit_valid(limit, codec) || !limit->width) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video orientation limits invalid codec=%d", codec); return -1;
    }
    unsigned side = limit->width > limit->height ? limit->width : limit->height;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "video verified orientations codec=%d source=%ux%u axes=%ux%u fps=%u pps=%u",
        codec, limit->width, limit->height, side, side, limit->fps, limit->pixels_per_second);
    limit->width = limit->height = side;
    return 0;
}
static int fits(const struct call_video_limit *l, const struct call_video_format *f) {
    return f->width <= l->width && f->height <= l->height && f->fps <= l->fps
        && (uint64_t)f->width * f->height * f->fps <= l->pixels_per_second && f->level <= l->level
        && f->maximum_bps <= l->maximum_bps && (f->profile & l->profiles);
}
int call_video_choose(const struct call_video_caps *sender, const struct call_video_caps *receiver,
                      struct call_video_format *format) {
    if (!call_video_caps_valid(sender) || !call_video_caps_valid(receiver) || !format || format->codec > 2
        || format->level || format->profile > 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video choose invalid capabilities/request"); return -1;
    }
    struct call_video_format f = *format;
    // Проверяем вход независимо от наличия codec у сторон, на самом высоком уровне.
    f.codec = CALL_VIDEO_H265; f.level = 186; f.profile = CALL_VIDEO_MAIN;
    if (!call_video_format_valid(&f)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video choose invalid size=%ux%u fps=%u bitrate=%u profile=%u",
                    f.width, f.height, f.fps, f.maximum_bps, format->profile);
        return -1;
    }
    for (int codec = 2; codec >= 1; codec--) {
        if (format->codec && codec != format->codec) continue;
        f.codec = codec;
        const struct call_video_limit *tx = &sender->encode[codec - 1], *rx = &receiver->decode[codec - 1];
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video mode limits codec=%u request=%ux%u@%u bps=%u "
            "encode=%ux%u@%u pps=%u level=%u bps=%u profiles=%u decode=%ux%u@%u pps=%u level=%u bps=%u profiles=%u",
            codec, f.width, f.height, f.fps, f.maximum_bps,
            tx->width, tx->height, tx->fps, tx->pixels_per_second, tx->level, tx->maximum_bps, tx->profiles,
            rx->width, rx->height, rx->fps, rx->pixels_per_second, rx->level, rx->maximum_bps, rx->profiles);
        unsigned profiles = sender->encode[codec - 1].profiles & receiver->decode[codec - 1].profiles;
        if (format->profile) profiles &= format->profile;
        if (!profiles) continue;
        f.profile = codec == CALL_VIDEO_H264 ? (profiles & CALL_VIDEO_CONSTRAINED_BASELINE ? CALL_VIDEO_CONSTRAINED_BASELINE : 1)
            : (profiles & CALL_VIDEO_MAIN ? CALL_VIDEO_MAIN : CALL_VIDEO_MAIN_HIGH_TIER);
        const struct video_level *levels = codec == CALL_VIDEO_H264 ? avc_levels : hevc_levels;
        unsigned count = codec == CALL_VIDEO_H264 ? sizeof(avc_levels) / sizeof(*levels) : sizeof(hevc_levels) / sizeof(*levels);
        for (unsigned i = 0; i < count; i++) {
            f.level = levels[i].id;
            if (!call_video_format_valid(&f) || !fits(&sender->encode[codec - 1], &f) || !fits(&receiver->decode[codec - 1], &f)) continue;
            *format = f;
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "video selected codec=%u level=%u size=%ux%u fps=%u bitrate_cap=%u profile=%u",
                       f.codec, f.level, f.width, f.height, f.fps, f.maximum_bps, f.profile); return codec;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video no common mode codec=%u size=%ux%u fps=%u bitrate_cap=%u profile=%u",
                format->codec, f.width, f.height, f.fps, f.maximum_bps, format->profile);
    return 0;
}
int call_video_format_valid(const struct call_video_format *f) {
    if (!(f && f->generation && f->codec >= 1 && f->codec <= 2 && f->width >= 16 && f->height >= 16
        && f->width <= 4096 && f->height <= 4096 && !(f->width % 2) && !(f->height % 2)
        && f->fps >= 1 && f->fps <= 60 && (f->rotation == 0 || f->rotation == 90 || f->rotation == 180 || f->rotation == 270)
        && f->maximum_bps >= CALL_VIDEO_RATE_MIN_BPS && f->maximum_bps <= CALL_VIDEO_RATE_MAX_BPS
        && (f->profile == 1 || f->profile == 2))) return 0;
    if (f->codec == CALL_VIDEO_H265 && f->profile == CALL_VIDEO_MAIN_HIGH_TIER && f->level < 120) return 0;
    const struct video_level *l = level_find(f->codec, f->level);
    if (!l) return 0;
    // AVC — macroblocks; HEVC резервируем padding до CTU 64, чтобы не зависеть от backend min-CU.
    uint64_t w = f->codec == CALL_VIDEO_H264 ? (f->width + 15u) / 16u : (f->width + 63u) / 64u * 64u;
    uint64_t h = f->codec == CALL_VIDEO_H264 ? (f->height + 15u) / 16u : (f->height + 63u) / 64u * 64u;
    return w * h <= l->picture && w * h * f->fps <= l->rate && w * w <= 8ull * l->picture && h * h <= 8ull * l->picture
        && f->maximum_bps <= (uint64_t)l->kbps * 1000;
}
unsigned call_video_dpb_limit(int codec, unsigned level, unsigned width, unsigned height) {
    const struct video_level *l = level <= 255 ? level_find(codec, level) : NULL;
    if (!l || !width || !height || width > 4096 || height > 4096) return 0;
    if (codec == CALL_VIDEO_H265) {
        uint64_t size = (uint64_t)width * height;
        if (size > l->picture) return 0;
        return size <= l->picture / 4 ? 16 : size <= l->picture / 2 ? 12 : size <= l->picture * 3 / 4 ? 8 : 6;
    }
    static const unsigned dpb_mbs[] = {396, 900, 2376, 2376, 2376, 4752, 8100, 8100, 18000,
        20480, 32768, 32768, 34816, 110400, 184320, 184320, 696320, 696320, 696320};
    unsigned frames = dpb_mbs[l - avc_levels] / (((width + 15) / 16) * ((height + 15) / 16));
    return frames < 16 ? frames : 16;
}
int call_video_hrd_valid(int codec, unsigned level, int nal, uint64_t bitrate, uint64_t cpb) {
    const struct video_level *l = level <= 255 ? level_find(codec, level) : NULL;
    unsigned factor = nal ? (codec == CALL_VIDEO_H264 ? 1200 : 1100) : 1000;
    return l && bitrate && cpb && bitrate <= (uint64_t)l->kbps * factor && cpb <= (uint64_t)l->cpb_kbits * factor;
}
int call_video_format_supported(const struct call_video_caps *sender, const struct call_video_caps *receiver,
                                const struct call_video_format *f) {
    return call_video_caps_valid(sender) && call_video_caps_valid(receiver) && call_video_format_valid(f)
        && fits(&sender->encode[f->codec - 1], f) && fits(&receiver->decode[f->codec - 1], f);
}
int call_video_same_format(const struct call_video_format *a, const struct call_video_format *b) {
    return a->generation == b->generation && a->width == b->width && a->height == b->height
        && a->fps == b->fps && a->rotation == b->rotation && a->codec == b->codec && a->level == b->level
        && a->maximum_bps == b->maximum_bps && a->profile == b->profile;
}
int call_video_caps_encode(const struct call_video_caps *caps, uint8_t out[CALL_VIDEO_CAPS_BYTES]) {
    if (!out || !call_video_caps_valid(caps)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video capabilities encode invalid arguments"); return -1;
    }
    for (unsigned i = 0; i < 4; i++) {
        const struct call_video_limit *l = i < 2 ? &caps->encode[i] : &caps->decode[i - 2];
        uint8_t *p = out + i * 16;
        write_be(p, l->width, 2); write_be(p + 2, l->height, 2);
        write_be(p + 4, l->fps, 2); write_be(p + 6, l->pixels_per_second, 4);
        write_be(p + 10, l->maximum_bps, 4); p[14] = l->level; p[15] = l->profiles;
    }
    return 0;
}
int call_video_caps_parse(const uint8_t *data, size_t length, struct call_video_caps *out) {
    if (!data || !out || length != CALL_VIDEO_CAPS_BYTES) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video capabilities invalid size=%zu", length); return -1;
    }
    struct call_video_caps caps = {0};
    for (unsigned i = 0; i < 4; i++) {
        struct call_video_limit *l = i < 2 ? &caps.encode[i] : &caps.decode[i - 2];
        const uint8_t *p = data + i * 16;
        l->width = read_be(p, 2); l->height = read_be(p + 2, 2);
        l->fps = read_be(p + 4, 2); l->pixels_per_second = read_be(p + 6, 4);
        l->maximum_bps = read_be(p + 10, 4); l->level = p[14]; l->profiles = p[15];
    }
    if (!call_video_caps_valid(&caps)) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "video capabilities invalid limits"); return -1; }
    *out = caps; return 0;
}
static int fragment_valid(const struct call_video_frame *f) {
    return f && f->call_id && f->frame_id && f->pts_us >= 0 && f->pts_us <= INT64_MAX / 2 && call_video_format_valid(&f->format)
        && !(f->flags & ~CALL_VIDEO_KEY) && f->data && f->size && f->size <= CALL_VIDEO_MAX_FRAME
        && f->offset < f->size && f->length && f->length <= CALL_VIDEO_FRAGMENT_BYTES && f->length <= f->size - f->offset;
}
int call_video_fragment_encode(const struct call_video_frame *f, uint8_t *out, size_t capacity, size_t *length) {
    if (!fragment_valid(f) || !out || !length || capacity < CALL_VIDEO_HEADER_BYTES + f->length) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video fragment encode invalid arguments"); return -1;
    }
    write_be(out, f->call_id, 8); write_be(out + 8, f->format.generation, 8);
    write_be(out + 16, f->frame_id, 8); write_be(out + 24, f->pts_us, 8);
    write_be(out + 32, f->format.width, 2); write_be(out + 34, f->format.height, 2);
    write_be(out + 36, f->format.fps, 2); write_be(out + 38, f->format.rotation, 2);
    out[40] = f->format.codec; out[41] = f->flags; out[42] = f->format.level; out[43] = f->format.profile;
    write_be(out + 44, f->size, 4); write_be(out + 48, f->offset, 4);
    write_be(out + 52, f->format.maximum_bps, 4);
    memcpy(out + CALL_VIDEO_HEADER_BYTES, f->data, f->length);
    *length = CALL_VIDEO_HEADER_BYTES + f->length; return 0;
}
int call_video_fragment_parse(const uint8_t *data, size_t length, struct call_video_frame *out) {
    if (!data || !out || length <= CALL_VIDEO_HEADER_BYTES || length > CALL_VIDEO_HEADER_BYTES + CALL_VIDEO_FRAGMENT_BYTES) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video fragment invalid size=%zu", length); return -1;
    }
    struct call_video_frame f = {0};
    f.call_id = read_be(data, 8); f.format.generation = read_be(data + 8, 8);
    f.frame_id = read_be(data + 16, 8); f.pts_us = (int64_t)read_be(data + 24, 8);
    f.format.width = read_be(data + 32, 2); f.format.height = read_be(data + 34, 2);
    f.format.fps = read_be(data + 36, 2); f.format.rotation = read_be(data + 38, 2);
    f.format.codec = data[40]; f.flags = data[41]; f.format.level = data[42];
    f.size = read_be(data + 44, 4); f.offset = read_be(data + 48, 4);
    f.format.maximum_bps = read_be(data + 52, 4); f.format.profile = data[43];
    f.length = length - CALL_VIDEO_HEADER_BYTES; f.data = data + CALL_VIDEO_HEADER_BYTES;
    if (!fragment_valid(&f)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video fragment invalid fields frame=%llu size=%u offset=%u bytes=%u",
                   (unsigned long long)f.frame_id, f.size, f.offset, f.length); return -1;
    }
    *out = f; return 0;
}

struct call_video_receiver {
    struct call_video_frame frame;
    uint8_t *data;
    uint32_t capacity, received;
    uint64_t last_us, last_id;
    int64_t last_pts;
    int need_key, active;
};
struct call_video_receiver *call_video_receiver_create(void) {
    struct call_video_receiver *r = u_calloc(1, sizeof(*r));
    if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video receiver allocation failed"); return NULL; }
    r->need_key = 1; r->last_pts = -1; return r;
}
void call_video_receiver_destroy(struct call_video_receiver *r) { if (r) { u_free(r->data); u_free(r); } }
void call_video_receiver_reset(struct call_video_receiver *r) {
    if (!r) return;
    u_free(r->data); memset(r, 0, sizeof(*r)); r->need_key = 1; r->last_pts = -1;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video receiver reset");
}
int call_video_receiver_needs_key(const struct call_video_receiver *r) { return !r || r->need_key; }
static int receiver_fail(struct call_video_receiver *r, const char *reason) {
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "video assembly rejected call=%llu generation=%llu frame=%llu key=%d received=%u total=%u reason=%s",
               (unsigned long long)r->frame.call_id, (unsigned long long)r->frame.format.generation,
               (unsigned long long)r->frame.frame_id, !!(r->frame.flags & CALL_VIDEO_KEY), r->received, r->frame.size, reason);
    r->active = 0; r->received = 0; r->need_key = 1; return -1;
}
int call_video_receiver_push(struct call_video_receiver *r, const struct call_video_frame *f,
                             uint64_t now_us, struct call_video_frame *complete) {
    if (!r || !complete || !fragment_valid(f)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video receiver invalid arguments"); return -1;
    }
    if (now_us < r->last_us) return receiver_fail(r, "clock_regression");
    r->last_us = now_us;
    if (!f->offset) {
        if (f->frame_id <= r->last_id || f->pts_us <= r->last_pts || f->format.generation < r->frame.format.generation)
            return receiver_fail(r, "stale_frame");
        if (r->active) receiver_fail(r, "incomplete_frame");
        if (r->last_id && f->frame_id != r->last_id + 1) r->need_key = 1;
        int changed = !call_video_same_format(&f->format, &r->frame.format);
        if (changed && f->format.generation == r->frame.format.generation) return receiver_fail(r, "format_without_generation");
        if (changed) r->need_key = 1;
        if (r->need_key && !(f->flags & CALL_VIDEO_KEY)) return 0;
        if (r->capacity < f->size) {
            uint8_t *data = u_realloc(r->data, f->size);
            if (!data) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video frame allocation failed bytes=%u", f->size); return receiver_fail(r, "allocation"); }
            r->data = data; r->capacity = f->size;
        }
        r->frame = *f; r->frame.data = r->data; r->received = 0; r->active = 1;
    }
    if (!r->active) {
        if (r->need_key && !(f->flags & CALL_VIDEO_KEY)) return 0;
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video fragment has no start call=%llu frame=%llu offset=%u key=%d",
                   (unsigned long long)f->call_id, (unsigned long long)f->frame_id, f->offset, !!(f->flags & CALL_VIDEO_KEY));
        r->need_key = 1; return -1;
    }
    if (f->call_id != r->frame.call_id || f->frame_id != r->frame.frame_id || f->pts_us != r->frame.pts_us
        || f->size != r->frame.size || f->flags != r->frame.flags || !call_video_same_format(&f->format, &r->frame.format)
        || f->offset != r->received) return receiver_fail(r, "fragment_mismatch");
    memcpy(r->data + r->received, f->data, f->length); r->received += f->length;
    if (r->received != r->frame.size) return 0;
    r->active = 0; r->last_id = f->frame_id; r->last_pts = f->pts_us;
    if (f->flags & CALL_VIDEO_KEY) {
        if (f->size < 5 || read_be(r->data, 4) > 65536 || read_be(r->data, 4) >= f->size - 4)
            return receiver_fail(r, "key_envelope");
        DEBUG_TRACE(DEBUG_CATEGORY_CALL, "video KEY assembled generation=%llu frame=%llu pts=%lld bytes=%u codec=%u size=%ux%u",
                    (unsigned long long)f->format.generation, (unsigned long long)f->frame_id, (long long)f->pts_us,
                    f->size, f->format.codec, f->format.width, f->format.height);
    }
    r->need_key = 0; r->frame.offset = 0; r->frame.length = r->frame.size; *complete = r->frame; return 1;
}
