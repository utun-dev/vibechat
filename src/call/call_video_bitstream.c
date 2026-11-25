#include "call_video_bitstream.h"
#include "call_video_fixtures.h"
#include "call_video_portrait_fixtures.h"
#include "call_video_fullhd_fixtures.h"
#include "call_video_fast_fixtures.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <string.h>

struct bits { const uint8_t *data; size_t size, at; int error; };
static unsigned get(struct bits *b, unsigned count) {
    if (count > 32 || b->at + count > b->size * 8) { b->error = 1; return 0; }
    unsigned value = 0;
    while (count--) { value = (value << 1) | ((b->data[b->at / 8] >> (7 - b->at % 8)) & 1); ++b->at; }
    return value;
}
static unsigned ue(struct bits *b) {
    unsigned zeros = 0;
    while (!b->error && !get(b, 1)) if (++zeros > 20) { b->error = 1; break; }
    return b->error ? 0 : (1u << zeros) - 1 + get(b, zeros);
}
static int se(struct bits *b) { unsigned v = ue(b); return v & 1 ? (int)((v + 1) / 2) : -(int)(v / 2); }
static int bad(const char *reason) {
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "video bitstream rejected reason=%s", reason); return -1;
}

/* RBSP без emulation prevention; некорректные escape и размеры отклоняются. */
static int rbsp(const uint8_t *data, size_t size, uint8_t *out, struct bits *bits) {
    unsigned zeros = 0; size_t n = 0;
    for (size_t i = 0; i < size; ++i) {
        if (zeros == 2 && data[i] == 3) {
            if (i + 1 == size || data[i + 1] > 3) return bad("emulation prevention");
            zeros = 0; continue;
        }
        out[n++] = data[i]; zeros = data[i] == 0 ? zeros + 1 : 0;
        if (zeros > 2) zeros = 2;
    }
    *bits = (struct bits){out, n, 0, 0}; return 0;
}
static size_t start_code(const uint8_t *data, size_t size, size_t at) {
    for (; at + 2 < size; ++at) if (!data[at] && !data[at + 1] && data[at + 2] == 1) return at;
    return size;
}

/* HRD AVC: проверяем заявленные BR/CPB по уровню, не обещаем их при отсутствии HRD. */
static void avc_hrd(struct bits *b, unsigned level, int nal) {
    unsigned count = ue(b) + 1, br_scale = get(b, 4), cpb_scale = get(b, 4);
    if (count > 32) { b->error = 1; return; }
    for (unsigned i = 0; i < count; ++i) {
        uint64_t bitrate = ((uint64_t)ue(b) + 1) << (6 + br_scale);
        uint64_t buffer = ((uint64_t)ue(b) + 1) << (4 + cpb_scale);
        get(b, 1);
        if (!call_video_hrd_valid(CALL_VIDEO_H264, level, nal, bitrate, buffer)) b->error = 1;
    }
    get(b, 20);
}
static int avc_sps(struct bits *b, struct call_video_bitstream *s, unsigned *id) {
    unsigned profile = get(b, 8), constraints = get(b, 8); s->level = get(b, 8);
    if (profile != 66 || (constraints & 3)) return bad("AVC profile/constraints");
    s->profile = constraints & 64 ? CALL_VIDEO_CONSTRAINED_BASELINE : CALL_VIDEO_BASELINE;
    *id = ue(b); if (*id > 31 || ue(b) > 12) return bad("AVC SPS id/frame_num");
    unsigned poc = ue(b);
    if (poc == 0) { if (ue(b) > 12) return bad("AVC POC size"); }
    else if (poc == 1) {
        get(b, 1); se(b); se(b); unsigned count = ue(b);
        if (count > 255) return bad("AVC POC cycle");
        for (unsigned i = 0; i < count; ++i) se(b);
    } else if (poc != 2) return bad("AVC POC type");
    s->dpb = ue(b); get(b, 1);
    unsigned w = ue(b) + 1, h = ue(b) + 1;
    if (w > 256 || h > 256 || !get(b, 1)) return bad("AVC coded geometry/interlace");
    get(b, 1); unsigned left = 0, right = 0, top = 0, bottom = 0;
    if (get(b, 1)) { left = ue(b); right = ue(b); top = ue(b); bottom = ue(b); }
    if ((uint64_t)(left + right) * 2 >= w * 16 || (uint64_t)(top + bottom) * 2 >= h * 16) return bad("AVC crop");
    s->width = w * 16 - (left + right) * 2; s->height = h * 16 - (top + bottom) * 2;
    unsigned limit = call_video_dpb_limit(CALL_VIDEO_H264, s->level, w * 16, h * 16);
    if (!limit || s->dpb > limit) return bad("AVC DPB");
    if (get(b, 1)) {
        if (get(b, 1)) { if (get(b, 8) == 255) { get(b, 16); get(b, 16); } }
        if (get(b, 1)) get(b, 1);
        if (get(b, 1)) { get(b, 4); if (get(b, 1)) get(b, 24); }
        if (get(b, 1)) { ue(b); ue(b); }
        if (get(b, 1)) { get(b, 32); get(b, 32); get(b, 1); }
        unsigned nal = get(b, 1); if (nal) avc_hrd(b, s->level, 1);
        unsigned vcl = get(b, 1); if (vcl) avc_hrd(b, s->level, 0);
        if (nal || vcl) get(b, 1);
        get(b, 1);
        if (get(b, 1)) {
            get(b, 1); ue(b); ue(b); ue(b); ue(b);
            unsigned reorder = ue(b), buffer = ue(b);
            if (reorder || buffer > limit || buffer < s->dpb) return bad("AVC reorder/DPB restriction");
        }
    }
    return b->error ? bad("truncated AVC SPS/HRD") : 0;
}
static int hevc_ptl(struct bits *b, struct call_video_bitstream *s) {
    unsigned space = get(b, 2), tier = get(b, 1), profile = get(b, 5);
    get(b, 32); // profile compatibility; принимаем только явный Main.
    get(b, 1); unsigned interlaced = get(b, 1); get(b, 2);
    get(b, 32); get(b, 12); s->level = get(b, 8); s->profile = tier ? CALL_VIDEO_MAIN_HIGH_TIER : CALL_VIDEO_MAIN;
    if (space || (tier && s->level < 120) || profile != 1 || interlaced) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video HEVC PTL rejected space=%u profile=%u tier=%u level=%u interlaced=%u",
            space, profile, tier, s->level, interlaced); return -1;
    }
    return b->error ? bad("truncated HEVC PTL") : 0;
}
/* HEVC HRD для единственного temporal layer; sub-picture HRD звонкам не требуется. */
static void hevc_hrd(struct bits *b, unsigned level) {
    unsigned nal = get(b, 1), vcl = get(b, 1), br_scale = 0, cpb_scale = 0;
    if (nal || vcl) {
        if (get(b, 1)) { b->error = 1; return; }
        br_scale = get(b, 4); cpb_scale = get(b, 4); get(b, 15);
    }
    unsigned fixed = get(b, 1); if (!fixed) fixed = get(b, 1);
    unsigned low_delay = 0;
    if (fixed) ue(b); else low_delay = get(b, 1);
    unsigned count = low_delay ? 1 : ue(b) + 1;
    if (count > 32) { b->error = 1; return; }
    for (unsigned kind = 0; kind < 2; ++kind) if (kind == 0 ? nal : vcl) {
        for (unsigned i = 0; i < count; ++i) {
            uint64_t bitrate = ((uint64_t)ue(b) + 1) << (6 + br_scale);
            uint64_t buffer = ((uint64_t)ue(b) + 1) << (4 + cpb_scale);
            get(b, 1);
            if (!call_video_hrd_valid(CALL_VIDEO_H265, level, kind == 0, bitrate, buffer)) b->error = 1;
        }
    }
}
/* Проходим до VUI: именно field_seq_flag определяет поля при неопределённых source flags. */
static int hevc_sps_tail(struct bits *b, unsigned poc, unsigned level, unsigned *ctb_log2) {
    unsigned min_cb = ue(b) + 3, diff_cb = ue(b), min_tb = ue(b) + 2, diff_tb = ue(b);
    unsigned inter = ue(b), intra = ue(b);
    if (min_cb > 6 || diff_cb > 3 || min_cb + diff_cb < 4 || min_cb + diff_cb > 6
        || min_tb > min_cb || diff_tb > 3 || min_tb + diff_tb > 5 || inter > 4 || intra > 4) return bad("HEVC block sizes");
    *ctb_log2 = min_cb + diff_cb;
    if (get(b, 1) && get(b, 1)) {
        for (unsigned size = 0; size < 4; ++size) for (unsigned matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
            if (!get(b, 1)) ue(b);
            else { if (size > 1) se(b); for (unsigned i = 0; i < (size == 0 ? 16u : 64u); ++i) se(b); }
        }
    }
    get(b, 2);
    if (get(b, 1)) { get(b, 8); ue(b); ue(b); get(b, 1); }
    unsigned sets = ue(b), previous = 0;
    if (sets > 64) return bad("HEVC RPS count");
    for (unsigned i = 0; i < sets; ++i) {
        unsigned count = 0;
        if (i && get(b, 1)) {
            get(b, 1); ue(b);
            for (unsigned j = 0; j <= previous; ++j) { unsigned used = get(b, 1); if (used || get(b, 1)) ++count; }
        } else {
            unsigned negative = ue(b), positive = ue(b); count = negative + positive;
            if (count > 16) return bad("HEVC RPS pictures");
            for (unsigned j = 0; j < count; ++j) { ue(b); get(b, 1); }
        }
        if (count > 16) return bad("HEVC predicted RPS pictures");
        previous = count;
    }
    if (get(b, 1)) {
        unsigned count = ue(b); if (count > 32) return bad("HEVC long-term references");
        for (unsigned i = 0; i < count; ++i) { get(b, poc + 4); get(b, 1); }
    }
    get(b, 2);
    if (get(b, 1)) {
        if (get(b, 1)) { if (get(b, 8) == 255) { get(b, 16); get(b, 16); } }
        if (get(b, 1)) get(b, 1);
        if (get(b, 1)) { get(b, 4); if (get(b, 1)) get(b, 24); }
        if (get(b, 1)) { ue(b); ue(b); }
        get(b, 1);
        if (get(b, 1)) return bad("HEVC field sequence");
        get(b, 1);
        if (get(b, 1)) { ue(b); ue(b); ue(b); ue(b); }
        if (get(b, 1)) {
            get(b, 32); get(b, 32); if (get(b, 1)) ue(b);
            if (get(b, 1)) hevc_hrd(b, level);
        }
        if (get(b, 1)) { get(b, 3); ue(b); ue(b); ue(b); ue(b); ue(b); }
    }
    if (get(b, 1) && get(b, 8)) return bad("HEVC SPS extensions");
    return b->error ? bad("truncated HEVC SPS/VUI/HRD") : 0;
}
static int hevc_sps(struct bits *b, struct call_video_bitstream *s, unsigned *id, unsigned *vps, unsigned *ctbs) {
    *vps = get(b, 4);
    if (get(b, 3) || !get(b, 1) || hevc_ptl(b, s)) return bad("HEVC layers/PTL");
    *id = ue(b); if (*id > 15 || ue(b) != 1) return bad("HEVC SPS id/chroma");
    unsigned w = ue(b), h = ue(b), left = 0, right = 0, top = 0, bottom = 0;
    if (!w || !h || w > 4096 || h > 4096) return bad("HEVC coded geometry");
    if (get(b, 1)) { left = ue(b); right = ue(b); top = ue(b); bottom = ue(b); }
    if ((uint64_t)(left + right) * 2 >= w || (uint64_t)(top + bottom) * 2 >= h) return bad("HEVC crop");
    s->width = w - (left + right) * 2; s->height = h - (top + bottom) * 2;
    unsigned luma = ue(b), chroma = ue(b), poc = ue(b);
    if (luma || chroma || poc > 12) return bad("HEVC bit depth/POC");
    get(b, 1); s->dpb = ue(b) + 1;
    unsigned reorder = ue(b); ue(b);
    unsigned limit = call_video_dpb_limit(CALL_VIDEO_H265, s->level, w, h);
    if (!limit || s->dpb > limit || reorder) return bad("HEVC DPB/reorder");
    unsigned ctb_log2;
    if (hevc_sps_tail(b, poc, s->level, &ctb_log2)) return -1;
    unsigned edge = 1u << ctb_log2;
    *ctbs = ((w + edge - 1) / edge) * ((h + edge - 1) / edge);
    return 0;
}

/* Одна активная VPS/SPS/PPS; повтор идентичных параметров в AU разрешён.
 * IDR и PPS id проверяются по slice header, а не по флагу платформенного API. */
int call_video_inspect_key(int codec, const uint8_t *data, size_t length, struct call_video_bitstream *out) {
    if ((codec != 1 && codec != 2) || !data || !out || length < 9 || length > CALL_VIDEO_MAX_FRAME) return bad("KEY arguments");
    uint32_t config = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
    if (!config || config > 65536 || config >= length - 4) return bad("KEY configuration length");
    uint8_t *scratch = u_malloc(length);
    if (!scratch) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video RBSP allocation failed bytes=%zu", length); return -1; }
    struct call_video_bitstream stream = {0}, vp = {0};
    unsigned sps_id = 0, pps_id = 0, vps_id = 0, sps_vps = 0, have_sps = 0, have_pps = 0, have_vps = 0, slices = 0;
    unsigned extra_bits = 0, dependent_slices = 0, output_flag = 0, ctbs = 0, previous_mb = 0; int result = -1;
    data += 4; length -= 4;
    size_t at = start_code(data, length, 0);
    if (at > 1) { bad("Annex B prefix"); goto done; }
    while (at < length) {
        size_t begin = at + 3, end = start_code(data, length, begin);
        at = end;
        while (end > begin && data[end - 1] == 0) --end;
        unsigned header = codec == 1 ? 1 : 2;
        if (end <= begin + header || (data[begin] & 128)) { bad("NAL header/size"); goto done; }
        unsigned type = codec == 1 ? data[begin] & 31 : (data[begin] >> 1) & 63;
        if (codec == 2 && ((data[begin] & 1) || data[begin + 1] != 1)) { bad("HEVC NAL layer/temporal id"); goto done; }
        struct bits b;
        if (rbsp(data + begin + header, end - begin - header, scratch, &b)) goto done;
        if (codec == 1 && type == 7) {
            struct call_video_bitstream parsed = {0}; unsigned id;
            if (avc_sps(&b, &parsed, &id)) goto done;
            if (have_sps && (memcmp(&parsed, &stream, sizeof(parsed)) || id != sps_id)) { bad("multiple AVC SPS"); goto done; }
            stream = parsed; sps_id = id; have_sps = 1;
        } else if (codec == 2 && type == 32) {
            struct call_video_bitstream parsed = {0};
            unsigned id = get(&b, 4); get(&b, 2);
            if (get(&b, 6) || get(&b, 3) || !get(&b, 1) || get(&b, 16) != 65535 || hevc_ptl(&b, &parsed)) {
                bad("HEVC VPS layers/PTL"); goto done;
            }
            get(&b, 1); unsigned dpb = ue(&b) + 1, reorder = ue(&b); ue(&b);
            if (b.error || reorder || dpb > 16) { bad("HEVC VPS DPB"); goto done; }
            parsed.dpb = dpb;
            if (have_vps && (id != vps_id || memcmp(&parsed, &vp, sizeof(vp)))) { bad("multiple HEVC VPS"); goto done; }
            if (get(&b, 6) || ue(&b)) { bad("HEVC VPS layer sets"); goto done; }
            if (get(&b, 1)) {
                get(&b, 32); get(&b, 32); if (get(&b, 1)) ue(&b);
                unsigned count = ue(&b);
                if (count > 1) { bad("HEVC VPS HRD count"); goto done; }
                if (count) { if (ue(&b)) { bad("HEVC VPS HRD layer"); goto done; } hevc_hrd(&b, parsed.level); }
            }
            if (get(&b, 1) || b.error) { bad("HEVC VPS extension/truncation/HRD"); goto done; }
            vp = parsed; vps_id = id; have_vps = 1;
        } else if (codec == 2 && type == 33) {
            struct call_video_bitstream parsed = {0}; unsigned id, vps, blocks;
            if (hevc_sps(&b, &parsed, &id, &vps, &blocks)) goto done;
            if (have_sps && (memcmp(&parsed, &stream, sizeof(parsed)) || id != sps_id || vps != sps_vps || blocks != ctbs)) {
                bad("multiple HEVC SPS"); goto done;
            }
            stream = parsed; sps_id = id; sps_vps = vps; ctbs = blocks; have_sps = 1;
        } else if ((codec == 1 && type == 8) || (codec == 2 && type == 34)) {
            unsigned id = ue(&b), sps = ue(&b);
            if (!have_sps || sps != sps_id || id > (codec == 1 ? 255u : 63u) || (have_pps && id != pps_id)) {
                bad("PPS/SPS reference"); goto done;
            }
            if (codec == 1) {
                unsigned entropy = get(&b, 1), fields = get(&b, 1), groups = ue(&b);
                if (entropy || fields || groups || ue(&b) > 15 || ue(&b) > 15) { bad("AVC PPS tools"); goto done; }
                get(&b, 1); get(&b, 2); se(&b); se(&b); se(&b); get(&b, 2);
                if (get(&b, 1)) { bad("AVC redundant pictures"); goto done; }
            } else {
                unsigned dependent = get(&b, 1), output = get(&b, 1), extra = get(&b, 3);
                if (have_pps && (dependent != dependent_slices || output != output_flag || extra != extra_bits)) {
                    bad("inconsistent HEVC PPS slice flags"); goto done;
                }
                dependent_slices = dependent; output_flag = output; extra_bits = extra;
            }
            if (b.error) { bad("truncated PPS"); goto done; }
            pps_id = id; have_pps = 1;
        } else if ((codec == 1 && type >= 1 && type <= 5) || (codec == 2 && type <= 31)) {
            if (!have_sps || !have_pps || begin < config) { bad("slice before configuration"); goto done; }
            if (codec == 1) {
                unsigned mb = ue(&b), slice = ue(&b), pps = ue(&b);
                if (type != 5 || slice > 9 || slice % 5 != 2 || pps != pps_id || (!slices && mb) || (slices && mb <= previous_mb)) {
                    bad("AVC KEY is not independent ordered IDR"); goto done;
                }
                previous_mb = mb;
            } else {
                if (type != 19 && type != 20) { bad("HEVC KEY is not IDR"); goto done; }
                unsigned first = get(&b, 1); get(&b, 1);
                if (ue(&b) != pps_id || (!slices && !first) || (slices && first)) { bad("HEVC IDR slice/PPS"); goto done; }
                // Разрешение dependent segments в PPS не делает первый слайс зависимым.
                unsigned dependent = 0;
                if (!first) {
                    if (dependent_slices) dependent = get(&b, 1);
                    unsigned address_bits = 0;
                    for (unsigned remaining = ctbs - 1; remaining; remaining >>= 1) ++address_bits;
                    unsigned address = get(&b, address_bits);
                    if (!address || address >= ctbs) { bad("HEVC IDR segment address"); goto done; }
                }
                if (!dependent) {
                    get(&b, extra_bits);
                    if (ue(&b) != 2) { bad("HEVC IDR slice type"); goto done; }
                    if (output_flag && !get(&b, 1)) { bad("HEVC IDR picture output disabled"); goto done; }
                }
            }
            if (b.error) { bad("truncated IDR"); goto done; }
            ++slices;
        }
    }
    if (!slices || !have_sps || !have_pps || (codec == 2 && (!have_vps || sps_vps != vps_id
        || vp.profile != stream.profile || vp.level != stream.level || stream.dpb > vp.dpb))) {
        bad("missing/inconsistent parameter sets or IDR"); goto done;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_CALL,
        "video bitstream verified codec=%d profile=%u level=%u size=%ux%u dpb=%u slices=%u dependent=%u output_flag=%u",
        codec, stream.profile, stream.level, stream.width, stream.height, stream.dpb, slices, dependent_slices, output_flag);
    *out = stream; result = 0;
done:
    u_free(scratch); return result;
}
int call_video_check_key(const struct call_video_format *f, const uint8_t *data, size_t length) {
    struct call_video_bitstream s;
    if (!f || call_video_inspect_key(f->codec, data, length, &s)) return -1;
    if (s.width != f->width || s.height != f->height || s.level > f->level
        || (s.profile != f->profile && !(f->codec == 1 && f->profile == 1 && s.profile == 2))) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video KEY mismatch actual=%ux%u profile=%u level=%u dpb=%u expected=%ux%u profile=%u level=%u",
            s.width, s.height, s.profile, s.level, s.dpb, f->width, f->height, f->profile, f->level); return -1;
    }
    return 0;
}
int call_video_probe_key(int codec, int profile, unsigned width, unsigned height, unsigned fps, uint8_t *out, size_t capacity) {
    const int portrait = height > width;
    const unsigned wide = portrait ? height : width, short_side = portrait ? width : height;
    const uint8_t *data = codec == 1 ? call_video_fixture_1 : call_video_fixture_2;
    size_t size = codec == 1 ? sizeof(call_video_fixture_1) : sizeof(call_video_fixture_2);
    if (portrait) {
        data = codec == 1 ? call_video_portrait_1 : call_video_portrait_2;
        size = codec == 1 ? sizeof(call_video_portrait_1) : sizeof(call_video_portrait_2);
    }
    if (wide == 1920 && short_side == 1080) {
        if (portrait) {
            data = codec == 1 ? call_video_fullhd_1080_1920_1 : call_video_fullhd_1080_1920_2;
            size = codec == 1 ? sizeof(call_video_fullhd_1080_1920_1) : sizeof(call_video_fullhd_1080_1920_2);
        } else {
            data = codec == 1 ? call_video_fullhd_1920_1080_1 : call_video_fullhd_1920_1080_2;
            size = codec == 1 ? sizeof(call_video_fullhd_1920_1080_1) : sizeof(call_video_fullhd_1920_1080_2);
        }
    } else if (wide != 1280 || short_side != 720) return bad("probe key geometry");
    if (fps == 60) {
        const uint8_t *frames[] = {call_video_fast_1280_720_1, call_video_fast_1280_720_2,
            call_video_fast_720_1280_1, call_video_fast_720_1280_2, call_video_fast_1920_1080_1,
            call_video_fast_1920_1080_2, call_video_fast_1080_1920_1, call_video_fast_1080_1920_2};
        const size_t sizes[] = {sizeof(call_video_fast_1280_720_1), sizeof(call_video_fast_1280_720_2),
            sizeof(call_video_fast_720_1280_1), sizeof(call_video_fast_720_1280_2), sizeof(call_video_fast_1920_1080_1),
            sizeof(call_video_fast_1920_1080_2), sizeof(call_video_fast_1080_1920_1), sizeof(call_video_fast_1080_1920_2)};
        unsigned index = (wide == 1920 ? 4 : 0) + (portrait ? 2 : 0) + (codec == 2);
        data = frames[index]; size = sizes[index];
    } else if (fps != 30) return bad("probe key fps");
    if ((codec != 1 && codec != 2) || (profile != 1 && profile != 2)
        || !out || capacity < size)
        return bad("probe key arguments");
    memcpy(out, data, size);
    if (codec == 1 && profile == 1) out[10] &= ~64u;
    if (codec == CALL_VIDEO_H265 && profile == CALL_VIDEO_MAIN_HIGH_TIER) {
        // Только эталонные пробы: Main tools и Main BR/CPB также соответствуют High tier level 4.
        // Повышение декларации уровня не меняет entropy payload. Производственные VPS/SPS не переписываем.
        unsigned patched = 0;
        size_t at = start_code(out, size, 4);
        while (at < size) {
            size_t begin = at + 3, end = start_code(out, size, begin); at = end;
            unsigned type = (out[begin] >> 1) & 63;
            if (type != 32 && type != 33) continue;
            unsigned profile_at = type == 32 ? 6 : 3, raw = 0, zeros = 0;
            for (size_t i = begin; i < end; ++i) {
                if (zeros == 2 && out[i] == 3) { zeros = 0; continue; }
                if (raw == profile_at) {
                    if (out[i] != 1) return bad("probe HEVC profile");
                    out[i] |= 32;
                }
                if (raw == profile_at + 11) { if (out[i] < 120) out[i] = 120; ++patched; break; }
                zeros = out[i] == 0 ? zeros + 1 : 0; ++raw;
            }
        }
        if (patched != 2) return bad("probe HEVC PTL count");
    }
    return (int)size;
}
