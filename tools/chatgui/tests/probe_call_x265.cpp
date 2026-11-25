/* Диагностика пригодности x265 для звонка: reconfigure или замена на IDR, 4M -> 300k -> 4M,
 * фиксированный Main 8-bit / Main tier / level 3.1, без lookahead/B-кадров.
 * Проверяем фактический выход, а не только return/getter reconfigure.
 * Запускается вручную; код 2 означает, что backend не прошёл пробу адаптации. */
#include <x265.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "debug_config.h"

struct Probe {
    const x265_api* api = nullptr;
    x265_param *parameters = nullptr, *actual = nullptr;
    x265_encoder *encoder = nullptr, *previous = nullptr;
    FILE* dump = nullptr;
    ~Probe() {
        if (dump && fclose(dump)) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 probe output close failed");
        if (encoder) api->encoder_close(encoder);
        if (previous) api->encoder_close(previous);
        if (parameters) api->param_free(parameters);
        if (actual) api->param_free(actual);
    }
};

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    bool frameRc = false, keyReconfig = false, keyReopen = false;
    const char* outputPath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--output") && i + 1 < argc && !outputPath) outputPath = argv[++i];
        else if (!frameRc && !keyReconfig && !keyReopen && !strcmp(argv[i], "--frame-rc")) frameRc = true;
        else if (!frameRc && !keyReconfig && !keyReopen && !strcmp(argv[i], "--keyframe-reconfig")) keyReconfig = true;
        else if (!frameRc && !keyReconfig && !keyReopen && !strcmp(argv[i], "--keyframe-reopen")) keyReopen = true;
        else {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL,
                        "Usage: probe_call_x265 [--frame-rc|--keyframe-reconfig|--keyframe-reopen] [--output path]"); return 1;
        }
    }
    bool keyMode = keyReconfig || keyReopen;
    Probe p; p.api = x265_api_get(8);
    if (outputPath) {
        p.dump = fopen(outputPath, "wb");
        if (!p.dump) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 probe output open failed path=%s", outputPath); return 1; }
    }
    if (!p.api || p.api->sizeof_param != sizeof(x265_param) || p.api->sizeof_picture != sizeof(x265_picture)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 8-bit API unavailable or ABI mismatch"); return 1;
    }
    p.parameters = p.api->param_alloc(); p.actual = p.api->param_alloc();
    if (!p.parameters || !p.actual || p.api->param_default_preset(p.parameters, "ultrafast", "zerolatency")) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 probe parameter allocation/preset failed"); return 1;
    }
    auto* config = p.parameters;
    config->sourceWidth = 640; config->sourceHeight = 360; config->fpsNum = 30; config->fpsDenom = 1;
    config->internalCsp = X265_CSP_I420; config->internalBitDepth = 8;
    config->levelIdc = 31; config->bHighTier = 0; config->maxNumReferences = 1;
    config->bframes = 0; config->lookaheadDepth = 0; config->frameNumThreads = 1;
    config->keyframeMax = 30; config->keyframeMin = 1; config->bOpenGOP = 0;
    config->bRepeatHeaders = keyMode || p.dump; config->bEmitInfoSEI = 0; config->bEmitHRDSEI = 0;
    config->rc.rateControlMode = X265_RC_ABR; config->rc.bitrate = 4000;
    config->rc.vbvMaxBitrate = 4000; config->rc.vbvBufferSize = 1000;
    config->logLevel = X265_LOG_WARNING;
    if (p.api->param_parse(config, "pools", "none")
        || (frameRc && p.api->param_parse(config, "frame-rc", "1")) || p.api->param_apply_profile(config, "main")) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 probe option/profile rejected frame_rc=%d", frameRc); return 1;
    }
    p.encoder = p.api->encoder_open(config);
    if (!p.encoder) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 probe open failed"); return 1; }
    DEBUG_INFO(DEBUG_CATEGORY_CALL,
               "x265 probe version=%s frame_rc=%d key_reconfig=%d key_reopen=%d size=640x360 fps=30 level=31 tier=main vbv_kbits=1000",
               p.api->version_str, frameRc, keyReconfig, keyReopen);
    std::vector<uint8_t> pixels(640 * 360 * 3 / 2, 128);
    uint32_t random = 1;
    bool passed = true;
    for (int phase = 0; phase < 3; phase++) {
        int requested = phase == 1 ? 300 : 4000;
        if (!keyMode) {
            p.api->encoder_parameters(p.encoder, config);
            config->rc.bitrate = config->rc.vbvMaxBitrate = requested;
        }
        int result = phase && !keyMode ? p.api->encoder_reconfig(p.encoder, config) : 0;
        if (result) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 reconfig failed phase=%d result=%d", phase, result); return 1; }
        uint64_t bytes = 0;
        int measureFrom = keyMode ? 60 : 30;
        for (int frame = 0; frame < 120; frame++) {
            // Запрос между ключевыми кадрами; переключение только перед следующим плановым IDR.
            if (keyMode && phase && frame == 7)
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "x265 queued bitrate phase=%d frame=%d requested_kbps=%d next_idr=30", phase, frame, requested);
            if (keyMode && phase && frame == 30) {
                p.api->encoder_parameters(p.encoder, config);
                config->rc.bitrate = config->rc.vbvMaxBitrate = requested;
                if (keyReopen) {
                    x265_encoder* candidate = p.api->encoder_open(config);
                    if (!candidate) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 reopen failed phase=%d", phase); return 1; }
                    p.previous = p.encoder; p.encoder = candidate;
                } else {
                    result = p.api->encoder_reconfig(p.encoder, config);
                    if (result) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 key reconfig failed phase=%d result=%d", phase, result); return 1; }
                }
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "x265 applying at IDR phase=%d frame=%d requested_kbps=%d reopen=%d wait_frames=23",
                           phase, frame, requested, keyReopen);
            }
            // Детерминированная сложная сцена: малая скорость не объясняется статичным изображением.
            for (int i = 0; i < 640 * 360; i++) {
                random = random * 1664525u + 1013904223u;
                pixels[i] = 16 + ((random >> 24) * 219 / 255);
            }
            x265_picture input, output;
            p.api->picture_init(config, &input); p.api->picture_init(config, &output);
            input.planes[0] = pixels.data(); input.planes[1] = pixels.data() + 640 * 360;
            input.planes[2] = pixels.data() + 640 * 360 * 5 / 4;
            input.stride[0] = 640; input.stride[1] = input.stride[2] = 320;
            input.pts = (phase * 120 + frame) * 1000000ll / 30;
            if (keyMode && frame % 30 == 0) input.sliceType = X265_TYPE_IDR;
            x265_nal* nals = nullptr; uint32_t count = 0;
            result = p.api->encoder_encode(p.encoder, &nals, &count, &input, &output);
            if (result != 1 || output.pts != input.pts) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 probe delay/error phase=%d frame=%d result=%d pts=%lld expected=%lld",
                            phase, frame, result, (long long)output.pts, (long long)input.pts); return 1;
            }
            bool idr = false, vps = false, sps = false, pps = false;
            for (unsigned i = 0; i < count; i++) {
                if (frame >= measureFrom) bytes += nals[i].sizeBytes;
                idr |= nals[i].type == NAL_UNIT_CODED_SLICE_IDR_W_RADL || nals[i].type == NAL_UNIT_CODED_SLICE_IDR_N_LP;
                vps |= nals[i].type == NAL_UNIT_VPS; sps |= nals[i].type == NAL_UNIT_SPS; pps |= nals[i].type == NAL_UNIT_PPS;
                if (p.dump && fwrite(nals[i].payload, 1, nals[i].sizeBytes, p.dump) != nals[i].sizeBytes) {
                    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 probe output write failed phase=%d frame=%d", phase, frame); return 1;
                }
            }
            if (keyMode && frame % 30 == 0 && !(idr && vps && sps && pps)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 incomplete IDR phase=%d frame=%d idr=%d vps=%d sps=%d pps=%d",
                            phase, frame, idr, vps, sps, pps); return 1;
            }
            if (p.previous) {
                p.api->encoder_close(p.previous); p.previous = nullptr;
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "x265 replacement committed phase=%d frame=%d pts=%lld idr=%d",
                           phase, frame, (long long)output.pts, idr);
            }
            if (frame < 3 || (keyMode && frame == 30)) {
                p.api->encoder_parameters(p.encoder, p.actual);
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "x265 probe phase=%d frame=%d requested_kbps=%d getter_kbps=%d vbv_kbps=%d level=%d",
                           phase, frame, requested, p.actual->rc.bitrate, p.actual->rc.vbvMaxBitrate, p.actual->levelIdc);
            }
        }
        uint64_t measured = bytes * 8 * 30 / (120 - measureFrom);
        bool matches = measured >= (uint64_t)requested * 500 && measured <= (uint64_t)requested * 1500;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "x265 probe phase=%d target_bps=%d measured_bps=%llu acceptable=%d",
                   phase, requested * 1000, (unsigned long long)measured, matches);
        if (!matches) passed = false;
    }
    if (p.dump && fflush(p.dump)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 probe output flush failed"); return 1; }
    if (!passed) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "x265 backend unsuitable: measured bitrate does not follow commands"); return 2;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "x265 adaptation probe PASS (camera/decoder/other modes still require validation)");
    return 0;
}
