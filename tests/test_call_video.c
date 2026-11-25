#include "call/call_video.h"
#include "call/call_video_bitstream.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "video:%d %s\n", __LINE__, #x); exit(1); } } while (0)

static struct call_video_format mode(int codec, unsigned width, unsigned height, unsigned fps) {
    return (struct call_video_format){1, width, height, fps, 0, codec, 0, 4000000, 0};
}
static int choose(const struct call_video_caps *a, const struct call_video_caps *b, int codec, unsigned w, unsigned h, unsigned fps) {
    struct call_video_format f = mode(codec, w, h, fps); return call_video_choose(a, b, &f);
}
static void capabilities(void) {
    struct call_video_caps a = {0}, b = {0}, decoded;
    struct call_video_limit limit = {1920, 1080, 60, 1920u * 1080u * 30u, 4000000, 1, 41};
    a.encode[0] = a.encode[1] = b.decode[0] = b.decode[1] = limit;
    a.encode[1].level = b.decode[1].level = 123;
    CHECK(choose(&a, &b, 0, 1280, 720, 30) == CALL_VIDEO_H265);
    CHECK(choose(&b, &a, 0, 1280, 720, 30) == 0);
    b.decode[1].pixels_per_second = 1280 * 720 * 15;
    CHECK(choose(&a, &b, 0, 1280, 720, 30) == CALL_VIDEO_H264);
    CHECK(choose(&a, &b, CALL_VIDEO_H265, 1280, 720, 30) == 0);
    CHECK(choose(&a, &b, 0, 1920, 1080, 60) == 0);
    CHECK(choose(&a, &b, 0, 1279, 720, 30) == -1);
    uint8_t wire[CALL_VIDEO_CAPS_BYTES];
    CHECK(call_video_caps_encode(&b, wire) == 0 && call_video_caps_parse(wire, sizeof(wire), &decoded) == 0);
    CHECK(decoded.decode[1].pixels_per_second == b.decode[1].pixels_per_second && !decoded.encode[0].width);
    CHECK(decoded.decode[1].maximum_bps == 4000000 && decoded.decode[1].profiles == 1 && decoded.decode[1].level == 123);
    CHECK(call_video_caps_parse(wire, 40, &decoded) == -1); // Старый wire format не поддерживается.
    wire[63] = 4; CHECK(call_video_caps_parse(wire, sizeof(wire), &decoded) == -1); wire[63] = 1;
    wire[0] = 0xff; struct call_video_caps saved = decoded;
    CHECK(call_video_caps_parse(wire, sizeof(wire), &decoded) == -1 && !memcmp(&saved, &decoded, sizeof(saved)));
    a.encode[1].profiles = CALL_VIDEO_MAIN_HIGH_TIER;
    b.decode[1].pixels_per_second = limit.pixels_per_second;
    CHECK(choose(&a, &b, CALL_VIDEO_H265, 640, 480, 30) == 0);
    b.decode[1].profiles |= CALL_VIDEO_MAIN_HIGH_TIER;
    struct call_video_format high = mode(CALL_VIDEO_H265, 640, 480, 30);
    CHECK(call_video_choose(&a, &b, &high) == CALL_VIDEO_H265 && high.profile == CALL_VIDEO_MAIN_HIGH_TIER && high.level == 120);
    CHECK(call_video_format_supported(&a, &b, &high));
    high.level = 93; CHECK(!call_video_format_valid(&high));
    b.decode[1].level = 93; CHECK(!call_video_caps_valid(&b));
    puts("video: directional capabilities, HEVC preference, AVC fallback, pixel-rate limits PASS");
}
static void portrait_limits(void) {
    struct call_video_caps sender = {0}, receiver = {0};
    for (int codec = 1; codec <= 2; codec++) {
        struct call_video_limit limit = {1280, 720, 30, 1280 * 720 * 30, 4000000, 1, codec == 1 ? 31 : 93};
        sender.encode[codec - 1] = receiver.decode[codec - 1] = limit;
        CHECK(choose(&sender, &receiver, codec, 720, 1280, 30) == 0);
        CHECK(call_video_limit_orientations(&sender.encode[codec - 1], codec) == 0);
        CHECK(choose(&sender, &receiver, codec, 720, 1280, 30) == 0);
        CHECK(call_video_limit_orientations(&receiver.decode[codec - 1], codec) == 0);
        CHECK(sender.encode[codec - 1].pixels_per_second == limit.pixels_per_second);
        CHECK(choose(&sender, &receiver, codec, 720, 1280, 30) == codec);
        CHECK(choose(&sender, &receiver, codec, 1280, 720, 30) == codec);
        CHECK(choose(&sender, &receiver, codec, 1280, 1280, 30) == 0);
        struct call_video_limit invalid = limit, saved;
        invalid.fps = 0; saved = invalid;
        CHECK(call_video_limit_orientations(&invalid, codec) == -1 && !memcmp(&invalid, &saved, sizeof(saved)));
        CHECK(call_video_limit_orientations(&limit, 3) == -1 && limit.height == 720);
        CHECK(call_video_limit_orientations(NULL, codec) == -1);
    }
    puts("video: portrait requires both directed CAPS, pixel-rate cap remains enforced PASS");
}
static void envelopes(void) {
    struct call_video_caps a = {0}, b = {0};
    a.encode[0] = b.decode[0] = (struct call_video_limit){1920, 1080, 60, 1920u * 1080u * 60u, 4000000, 3, 42};
    a.encode[1] = b.decode[1] = (struct call_video_limit){1920, 1080, 60, 1920u * 1080u * 60u, 4000000, 1, 123};
    struct call_video_format f = mode(0, 1280, 720, 30);
    CHECK(call_video_choose(&a, &b, &f) == CALL_VIDEO_H265 && f.level == 93);
    CHECK(call_video_format_supported(&a, &b, &f) && !call_video_format_supported(&b, &a, &f));
    b.decode[1].level = 90;
    f = mode(0, 1280, 720, 30); CHECK(call_video_choose(&a, &b, &f) == CALL_VIDEO_H264 && f.level == 31);
    f = mode(CALL_VIDEO_H265, 1280, 720, 30); struct call_video_format saved = f;
    CHECK(call_video_choose(&a, &b, &f) == 0 && !memcmp(&f, &saved, sizeof(f)));
    b.decode[1].level = 123; b.decode[1].maximum_bps = 3999999;
    CHECK(choose(&a, &b, 0, 1280, 720, 30) == CALL_VIDEO_H264);
    b.decode[1].maximum_bps = 4000000;
    a.encode[0].profiles = 1; b.decode[0].profiles = 2;
    CHECK(choose(&a, &b, CALL_VIDEO_H264, 1280, 720, 30) == 0);
    b.decode[0].profiles = 3;
    f = mode(CALL_VIDEO_H264, 1280, 720, 30);
    CHECK(call_video_choose(&a, &b, &f) == CALL_VIDEO_H264 && f.profile == CALL_VIDEO_BASELINE);
    f = mode(CALL_VIDEO_H264, 176, 144, 30); f.maximum_bps = 300000;
    CHECK(call_video_choose(&a, &b, &f) == CALL_VIDEO_H264 && f.level == 12);
    f.level = 11; CHECK(!call_video_format_valid(&f)); // Геометрия проходит, битрейт — нет.
    CHECK(call_video_dpb_limit(CALL_VIDEO_H265, 93, 1280, 720) == 6);
    CHECK(call_video_dpb_limit(CALL_VIDEO_H265, 93, 640, 360) == 16);
    CHECK(call_video_dpb_limit(CALL_VIDEO_H264, 31, 1280, 720) == 5);
    CHECK(call_video_dpb_limit(CALL_VIDEO_H265, 90, 1280, 720) == 0);
    f = mode(CALL_VIDEO_H264, 1920, 1080, 60);
    CHECK(call_video_choose(&a, &b, &f) == CALL_VIDEO_H264 && f.level == 42);
    f.level = 41; CHECK(!call_video_format_valid(&f));
    f = mode(CALL_VIDEO_H265, 1920, 1080, 60);
    CHECK(call_video_choose(&a, &b, &f) == CALL_VIDEO_H265 && f.level == 123);
    f.level = 120; CHECK(!call_video_format_valid(&f));
    a.encode[0].level = 99; CHECK(!call_video_caps_valid(&a));
    puts("video: negotiated profiles/level, bitrate fallback, DPB/frame-rate boundaries, atomic selection PASS");
}
static struct call_video_frame frame(uint64_t id, uint64_t generation, const uint8_t *data, uint32_t size, int key) {
    struct call_video_frame f = {0};
    f.call_id = 42; f.frame_id = id; f.pts_us = 1000000 + id * 33333;
    f.format = (struct call_video_format){generation, 640, 480, 30, 90, CALL_VIDEO_H265, 93, 4000000, 1};
    f.size = size; f.length = size < CALL_VIDEO_FRAGMENT_BYTES ? size : CALL_VIDEO_FRAGMENT_BYTES;
    f.flags = key ? CALL_VIDEO_KEY : 0; f.data = data; return f;
}
static void roundtrip(void) {
    uint8_t data[5001], wire[CALL_VIDEO_HEADER_BYTES + CALL_VIDEO_FRAGMENT_BYTES];
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    memset(data, 0, 4);
    struct call_video_receiver *r = call_video_receiver_create(); CHECK(r);
    struct call_video_frame f = frame(1, 1, data, sizeof(data), 1), parsed, complete;
    CHECK(call_video_receiver_needs_key(r));
    for (unsigned offset = 0; offset < sizeof(data); offset += f.length) {
        f.offset = offset; f.data = data + offset;
        f.length = sizeof(data) - offset < CALL_VIDEO_FRAGMENT_BYTES ? sizeof(data) - offset : CALL_VIDEO_FRAGMENT_BYTES;
        size_t length = 0;
        CHECK(call_video_fragment_encode(&f, wire, sizeof(wire), &length) == 0);
        CHECK(wire[7] == 42 && wire[40] == 2 && wire[39] == 90 && wire[42] == 93);
        CHECK(call_video_fragment_parse(wire, length, &parsed) == 0);
        CHECK(parsed.frame_id == f.frame_id && parsed.pts_us == f.pts_us && parsed.offset == offset);
        CHECK(parsed.format.maximum_bps == 4000000 && parsed.format.profile == 1 && parsed.format.level == 93);
        int done = call_video_receiver_push(r, &parsed, offset, &complete);
        CHECK(done == (offset + f.length == sizeof(data) ? 1 : 0));
    }
    CHECK(!call_video_receiver_needs_key(r) && complete.size == sizeof(data) && complete.length == sizeof(data));
    CHECK(!memcmp(complete.data, data, sizeof(data)));
    f = frame(2, 1, data, 8, 0); CHECK(call_video_receiver_push(r, &f, 10000, &complete) == 1);
    f = frame(3, 2, data, 8, 0); CHECK(call_video_receiver_push(r, &f, 20000, &complete) == 0);
    f = frame(4, 2, data, 8, 1); f.format.codec = CALL_VIDEO_H264; f.format.level = 31;
    CHECK(call_video_receiver_push(r, &f, 30000, &complete) == 1 && complete.format.codec == CALL_VIDEO_H264);
    f = frame(5, 2, data, 8, 1); // Codec меняется без нового поколения.
    CHECK(call_video_receiver_push(r, &f, 40000, &complete) == -1);
    f.format.codec = CALL_VIDEO_H264; f.format.level = 31; f.format.profile = 2;
    CHECK(call_video_receiver_push(r, &f, 50000, &complete) == -1); // Профиль тоже часть generation.
    f.format.generation = 3;
    CHECK(call_video_receiver_push(r, &f, 60000, &complete) == 1);
    call_video_receiver_destroy(r);
    puts("video: endian wire, fragmented KEY, delta frames, ordered format switch PASS");
}
static void rejected(void) {
    uint8_t data[2400] = {0}, wire[CALL_VIDEO_HEADER_BYTES + CALL_VIDEO_FRAGMENT_BYTES];
    struct call_video_receiver *r = call_video_receiver_create(); CHECK(r);
    struct call_video_frame f = frame(1, 1, data, sizeof(data), 1), out = {0}, saved;
    CHECK(call_video_receiver_push(r, &f, 0, &out) == 0);
    f.offset = 1201; f.length = 1199;
    CHECK(call_video_receiver_push(r, &f, 10000, &out) == -1 && call_video_receiver_needs_key(r));
    f = frame(2, 1, data, sizeof(data), 1); CHECK(call_video_receiver_push(r, &f, 20000, &out) == 0);
    f.offset = 1200; CHECK(call_video_receiver_push(r, &f, 3020001, &out) == 1);
    CHECK(!call_video_receiver_needs_key(r) && out.length == sizeof(data));
    f = frame(3, 1, data, 8, 0); CHECK(call_video_receiver_push(r, &f, 3030000, &out) == 1);
    CHECK(call_video_receiver_push(r, &f, 3040000, &out) == -1); // Replay.
    call_video_receiver_reset(r); CHECK(call_video_receiver_needs_key(r));
    f = frame(4, 1, data, sizeof(data), 1); f.offset = 1200;
    CHECK(call_video_receiver_push(r, &f, 0, &out) == -1); // Потеря начала KEY.
    f = frame(5, 1, data, 8, 1);
    data[0] = 0xff; CHECK(call_video_receiver_push(r, &f, 0, &out) == -1); // Неверный config_length.
    data[0] = 0;
    size_t length = 0; CHECK(call_video_fragment_encode(&f, wire, sizeof(wire), &length) == 0);
    saved = out;
    for (size_t n = 0; n <= CALL_VIDEO_HEADER_BYTES; n++) CHECK(call_video_fragment_parse(wire, n, &out) == -1);
    wire[43] = 2; CHECK(call_video_fragment_parse(wire, length, &out) == -1);
    CHECK(!memcmp(&out, &saved, sizeof(saved))); wire[43] = 1;
    wire[44] = 0xff; CHECK(call_video_fragment_parse(wire, length, &out) == -1);
    f.size = CALL_VIDEO_MAX_FRAME + 1; CHECK(call_video_receiver_push(r, &f, 0, &out) == -1);
    call_video_receiver_destroy(r);
    puts("video: gap, pause with preserved KEY and delta, replay, malformed KEY, invalid packets PASS");
}
static void bitstreams(void) {
    uint8_t bytes[8192]; struct call_video_bitstream s;
    for (int codec = 1; codec <= 2; ++codec) {
        for (int profile = 1; profile <= 2; ++profile)
        for (unsigned fps = 30; fps <= 60; fps += 30)
        for (int fullhd = 0; fullhd <= 1; ++fullhd) for (int portrait = 0; portrait <= 1; portrait++) {
            unsigned w = fullhd ? 1920 : 1280, h = fullhd ? 1080 : 720;
            int size = call_video_probe_key(codec, profile, portrait ? h : w, portrait ? w : h, fps, bytes, sizeof(bytes)); CHECK(size > 0);
            int inspected = call_video_inspect_key(codec, bytes, size, &s);
            if (inspected) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "fixture rejected codec=%d profile=%d size=%ux%u bytes=%d",
                codec, profile, portrait ? h : w, portrait ? w : h, size);
            CHECK(!inspected);
            CHECK(s.width == (portrait ? h : w) && s.height == (portrait ? w : h)
                && s.profile == (unsigned)profile && s.level == (codec == 1
                    ? (fullhd ? (fps == 60 ? 42u : 40u) : (fps == 60 ? 32u : 31u))
                    : (fullhd ? (fps == 60 ? 123u : 120u) : (fps == 60 || profile == 2 ? 120u : 93u))));
            CHECK(s.dpb >= 3);
            struct call_video_format f = {1, s.width, s.height, fps, 0, codec, s.level, 4000000, profile};
            CHECK(!call_video_check_key(&f, bytes, size));
            if (codec == CALL_VIDEO_H265 && profile == CALL_VIDEO_MAIN_HIGH_TIER) {
                f.profile = CALL_VIDEO_MAIN; CHECK(call_video_check_key(&f, bytes, size) < 0); f.profile = profile;
            }
            f.width = 640; CHECK(call_video_check_key(&f, bytes, size) < 0);
            struct call_video_bitstream saved = s;
            for (int i = 0; i < 32; ++i) {
                CHECK(call_video_inspect_key(codec, bytes, i, &s) < 0);
                CHECK(!memcmp(&saved, &s, sizeof(s)));
            }
            bytes[0] = 255; CHECK(call_video_inspect_key(codec, bytes, size, &s) < 0);
        }
    }
    puts("video: real AVC Baseline/CB and HEVC IDR, multi-picture DPB, atomic malformed parse PASS");
}
static void put_bits(uint8_t *data, unsigned *at, unsigned value, unsigned count) {
    while (count--) { data[*at / 8] |= ((value >> count) & 1u) << (7 - *at % 8); ++*at; }
}
/* Только синтаксис заголовков: entropy payload здесь не нужен. SPS fixture: CTU32, 40*23 CTB, address=10 bits. */
static size_t hevc_segment(uint8_t *data, unsigned first, unsigned enabled, unsigned dependent,
                           unsigned output, unsigned visible, unsigned extra, unsigned address) {
    memset(data, 0, 16); data[2] = 1; data[3] = 40; data[4] = 1;
    unsigned at = 40;
    put_bits(data, &at, first, 1); put_bits(data, &at, 0, 1); put_bits(data, &at, 1, 1); // first, no_output_prior, PPS id=0.
    if (!first) {
        if (enabled) put_bits(data, &at, dependent, 1);
        put_bits(data, &at, address, 10);
    }
    if (!dependent) {
        put_bits(data, &at, 0, extra); put_bits(data, &at, 3, 3); // reserved bits, I slice ue(2).
        if (output) put_bits(data, &at, visible, 1);
    }
    put_bits(data, &at, 1, 1);
    return (at + 7) / 8;
}
static void hevc_pps_flags(void) {
    uint8_t bytes[8192]; struct call_video_bitstream stream;
    int size = call_video_probe_key(CALL_VIDEO_H265, CALL_VIDEO_MAIN, 1280, 720, 30, bytes, sizeof(bytes)); CHECK(size > 0);
    size_t pps = 0, config = 4 + ((unsigned)bytes[2] << 8 | bytes[3]);
    for (size_t i = 4; i + 5 < config; ++i)
        if (!bytes[i] && !bytes[i + 1] && bytes[i + 2] == 1 && bytes[i + 3] == 68) pps = i + 5;
    CHECK(pps && bytes[pps] == 0xc0); // Оба id=0, slice flags отсутствуют.
    bytes[pps] |= 0x20; // Реальный IDR остаётся валиден, когда PPS разрешает dependent segments.
    CHECK(!call_video_inspect_key(CALL_VIDEO_H265, bytes, size, &stream));
    for (unsigned flags = 0; flags < 4; ++flags) for (unsigned extra = 0; extra <= 3; extra += 3) {
        unsigned dependent = flags & 1, output = flags >> 1;
        bytes[pps] = 0xc0 | dependent << 5 | output << 4 | extra << 1;
        size_t end = config + hevc_segment(bytes + config, 1, dependent, 0, output, 1, extra, 0);
        CHECK(!call_video_inspect_key(CALL_VIDEO_H265, bytes, end, &stream));
        size_t next = end + hevc_segment(bytes + end, 0, dependent, 0, output, 1, extra, 1);
        CHECK(!call_video_inspect_key(CALL_VIDEO_H265, bytes, next, &stream));
        next = end + hevc_segment(bytes + end, 0, dependent, dependent, output, 1, extra, 1);
        CHECK(!call_video_inspect_key(CALL_VIDEO_H265, bytes, next, &stream));
        for (unsigned address = 0; address <= 1023; address += 1023) {
            next = end + hevc_segment(bytes + end, 0, dependent, dependent, output, 1, extra, address);
            CHECK(call_video_inspect_key(CALL_VIDEO_H265, bytes, next, &stream) < 0);
        }
        if (output) {
            next = end + hevc_segment(bytes + end, 0, dependent, 0, output, 0, extra, 1);
            CHECK(call_video_inspect_key(CALL_VIDEO_H265, bytes, next, &stream) < 0);
            end = config + hevc_segment(bytes + config, 1, dependent, 0, output, 0, extra, 0);
            CHECK(call_video_inspect_key(CALL_VIDEO_H265, bytes, end, &stream) < 0);
        }
    }
    puts("video: HEVC PPS flags, independent/dependent IDR segments, output flag and CTB address validation PASS");
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    capabilities(); portrait_limits(); envelopes(); roundtrip(); rejected(); bitstreams(); hevc_pps_flags(); return 0;
}
