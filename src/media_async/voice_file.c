/* Общий кодек файла и waveform без зависимости от GUI, беседы или сети. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../lib/debug_config.h"
#include "../../lib/opus_codec.h"
#include "voice_file.h"

int voice_file_encode(const int16_t* pcm, size_t count, int rate, int channels, int preset,
                       const char* path, struct attachment_info* info) {
    static const int bitrates[] = {16000, 32000, 64000}, complexities[] = {2, 5, 10};
    static const char levels[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!pcm || !info || !path || (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) ||
        (channels != 1 && channels != 2) || preset < 0 || preset > 2) goto invalid;
    size_t frame = (size_t)(rate / 50) * channels, frames = count / frame;
    if (frames < 25 || frames > ATTACHMENT_DURATION_MAX_MS / 20) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA,
                   "voice_file: invalid duration count=%zu rate=%d channels=%d frames=%zu allowed_frames=25..%u",
                   count, rate, channels, frames, ATTACHMENT_DURATION_MAX_MS / 20);
        return -1;
    }
    size_t samples = frames * frame;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "voice_file: encode count=%zu rate=%d channels=%d frames=%zu duration=%ums discarded=%zu",
                count, rate, channels, frames, (uint32_t)(frames * 20), count - samples);
    opus_codec_encoder_t* encoder = opus_codec_encoder_create(rate, channels);
    if (!encoder) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice_file: encoder creation failed"); return -1; }
    opus_codec_encoder_bitrate_set(encoder, bitrates[preset]);
    opus_codec_encoder_complexity_set(encoder, complexities[preset]);
    FILE* file = fopen(path, "wb");
    int error = -1;
    if (!file) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice_file: output open failed path=%s", path); goto done; }
    uint32_t magic = 0x5355504f, sample_rate = (uint32_t)rate;
    uint16_t channel_count = (uint16_t)channels, frame_samples = (uint16_t)(rate / 50);
    if (fwrite(&magic, 4, 1, file) != 1 || fwrite(&sample_rate, 4, 1, file) != 1 ||
        fwrite(&channel_count, 2, 1, file) != 1 || fwrite(&frame_samples, 2, 1, file) != 1) goto done;
    uint8_t packet[4000];
    for (size_t offset = 0; offset < samples; offset += frame) {
        int n = opus_codec_encode(encoder, pcm + offset, frame_samples, packet, sizeof(packet));
        if (n <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice_file: encode failed offset=%zu result=%d", offset, n); goto done; }
        uint16_t length = (uint16_t)n;
        if (fwrite(&length, 2, 1, file) != 1 || fwrite(packet, 1, (size_t)n, file) != (size_t)n) goto done;
    }
    uint16_t end = 0;
    if (fwrite(&end, 2, 1, file) != 1 || fflush(file)) goto done;
    info->kind = ATTACHMENT_VOICE; info->duration_ms = (uint32_t)(frames * 20);
    info->width = info->height = 0;
    for (size_t i = 0; i < 100; i++) {
        size_t first = i * samples / 100, last = (i + 1) * samples / 100;
        double sum = 0;
        for (size_t j = first; j < last; j++) { double v = pcm[j] / 32768.0; sum += v * v; }
        double rms = sqrt(sum / (last - first)), db = rms > 0.00001 ? 20 * log10(rms) : -100;
        int level = (int)((db + 60) * 64 / 60);
        if (level < 0) level = 0;
        if (level > 63) level = 63;
        info->waveform[i] = levels[level];
    }
    info->waveform[100] = 0;
    error = 0;
done:
    if (file && fclose(file)) error = -1;
    opus_codec_encoder_destroy(encoder);
    if (error) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice_file: encoding/write failed path=%s", path);
        if (file && remove(path)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice_file: partial file cleanup failed path=%s", path);
    } else DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "voice_file: prepared path=%s duration=%ums rate=%d channels=%d", path, info->duration_ms, rate, channels);
    return error;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice_file: invalid parameters count=%zu rate=%d channels=%d preset=%d", count, rate, channels, preset);
    return -1;
}
