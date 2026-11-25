#include <stdlib.h>
#include <string.h>
#include "opus_codec.h"
#include "opus.h"
#include "debug_config.h"

#define OPUS_DEFAULT_SAMPLE_RATE   48000
#define OPUS_DEFAULT_BITRATE       32000
#define OPUS_DEFAULT_COMPLEXITY    5
#define OPUS_DEFAULT_FRAME_MS      20

struct opus_codec_encoder {
    OpusEncoder* opus_enc;
    int sample_rate;
    int channels;
    int frame_samples;
};

struct opus_codec_decoder {
    OpusDecoder* opus_dec;
    int sample_rate;
    int channels;
};

static int frame_samples_for_rate(int sample_rate) {
    return sample_rate * OPUS_DEFAULT_FRAME_MS / 1000;
}

opus_codec_encoder_t* opus_codec_encoder_create(int sample_rate, int channels) {
    int err;
    opus_codec_encoder_t* enc;

    if (sample_rate <= 0) sample_rate = OPUS_DEFAULT_SAMPLE_RATE;
    if (channels <= 0 || channels > 2) channels = 1;

    enc = (opus_codec_encoder_t*)calloc(1, sizeof(*enc));
    if (!enc) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "opus_codec_encoder_create: OOM");
        return NULL;
    }

    enc->sample_rate = sample_rate;
    enc->channels = channels;
    enc->frame_samples = frame_samples_for_rate(sample_rate);

    enc->opus_enc = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "opus_codec_encoder_create: opus_encoder_create failed: %s (%d)",
                opus_codec_strerror(err), err);
        free(enc);
        return NULL;
    }

    opus_encoder_ctl(enc->opus_enc, OPUS_SET_BITRATE(OPUS_DEFAULT_BITRATE));
    opus_encoder_ctl(enc->opus_enc, OPUS_SET_COMPLEXITY(OPUS_DEFAULT_COMPLEXITY));
    opus_encoder_ctl(enc->opus_enc, OPUS_SET_SIGNAL(OPUS_AUTO));

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "opus_codec_encoder_create: ok rate=%d ch=%d frame_samples=%d bitrate=%d",
            sample_rate, channels, enc->frame_samples, OPUS_DEFAULT_BITRATE);
    return enc;
}

void opus_codec_encoder_destroy(opus_codec_encoder_t* enc) {
    if (!enc) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "opus_codec_encoder_destroy");
    if (enc->opus_enc) opus_encoder_destroy(enc->opus_enc);
    free(enc);
}

int opus_codec_encode(opus_codec_encoder_t* enc, const int16_t* pcm, int frame_samples,
        uint8_t* out, int out_cap) {
    if (!enc || !pcm || !out || out_cap <= 0 || frame_samples <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "opus_codec_encode: invalid args");
        return -1;
    }
    int result = opus_encode(enc->opus_enc, pcm, frame_samples, out, out_cap);
    if (result < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "opus_codec_encode: encode failed: %s (%d), pcm_samples=%d cap=%d",
                opus_codec_strerror(result), result, frame_samples, out_cap);
    }
    return result;
}

opus_codec_decoder_t* opus_codec_decoder_create(int sample_rate, int channels) {
    int err;
    opus_codec_decoder_t* dec;

    if (sample_rate <= 0) sample_rate = OPUS_DEFAULT_SAMPLE_RATE;
    if (channels <= 0 || channels > 2) channels = 1;

    dec = (opus_codec_decoder_t*)calloc(1, sizeof(*dec));
    if (!dec) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "opus_codec_decoder_create: OOM");
        return NULL;
    }

    dec->sample_rate = sample_rate;
    dec->channels = channels;

    dec->opus_dec = opus_decoder_create(sample_rate, channels, &err);
    if (err != OPUS_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "opus_codec_decoder_create: opus_decoder_create failed: %s (%d)",
                opus_codec_strerror(err), err);
        free(dec);
        return NULL;
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "opus_codec_decoder_create: ok rate=%d ch=%d",
            sample_rate, channels);
    return dec;
}

void opus_codec_decoder_destroy(opus_codec_decoder_t* dec) {
    if (!dec) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "opus_codec_decoder_destroy");
    if (dec->opus_dec) opus_decoder_destroy(dec->opus_dec);
    free(dec);
}

int opus_codec_decode(opus_codec_decoder_t* dec, const uint8_t* data, int len,
        int16_t* pcm, int frame_samples) {
    if (!dec || !pcm || frame_samples <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "opus_codec_decode: invalid args");
        return -1;
    }
    int result = opus_decode(dec->opus_dec, data, len, pcm, frame_samples, 0);
    if (result < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "opus_codec_decode: decode failed: %s (%d), len=%d",
                opus_codec_strerror(result), result, len);
    }
    return result;
}

int opus_codec_encoder_bitrate_set(opus_codec_encoder_t* enc, int bitrate_bps) {
    if (!enc || !enc->opus_enc) return -1;
    return opus_encoder_ctl(enc->opus_enc, OPUS_SET_BITRATE(bitrate_bps));
}

int opus_codec_encoder_complexity_set(opus_codec_encoder_t* enc, int complexity) {
    if (!enc || !enc->opus_enc) return -1;
    return opus_encoder_ctl(enc->opus_enc, OPUS_SET_COMPLEXITY(complexity));
}

int opus_codec_encoder_signal_set(opus_codec_encoder_t* enc, int signal_type) {
    if (!enc || !enc->opus_enc) return -1;
    return opus_encoder_ctl(enc->opus_enc, OPUS_SET_SIGNAL(signal_type));
}

int opus_codec_encoder_application_set(opus_codec_encoder_t* enc, int application) {
    if (!enc || !enc->opus_enc) return -1;
    return opus_encoder_ctl(enc->opus_enc, OPUS_SET_APPLICATION(application));
}

int opus_codec_encoder_delay(opus_codec_encoder_t* enc, int* samples) {
    if (!enc || !samples) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Opus delay invalid arguments"); return -1; }
    opus_int32 delay = 0;
    int result = opus_encoder_ctl(enc->opus_enc, OPUS_GET_LOOKAHEAD(&delay));
    if (result != OPUS_OK || delay < 0 || delay > enc->sample_rate) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Opus delay query failed result=%d samples=%d", result, (int)delay); return -1;
    }
    *samples = delay;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "Opus capture delay samples=%d rate=%d", (int)delay, enc->sample_rate);
    return 0;
}

const char* opus_codec_strerror(int err) {
    switch (err) {
    case OPUS_OK: return "OK";
    case OPUS_BAD_ARG: return "BAD_ARG";
    case OPUS_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case OPUS_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case OPUS_INVALID_PACKET: return "INVALID_PACKET";
    case OPUS_UNIMPLEMENTED: return "UNIMPLEMENTED";
    case OPUS_INVALID_STATE: return "INVALID_STATE";
    case OPUS_ALLOC_FAIL: return "ALLOC_FAIL";
    default: return "UNKNOWN";
    }
}
