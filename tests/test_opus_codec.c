#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "opus_codec.h"

#define SAMPLE_RATE 48000
#define CHANNELS    1
#define FRAME_MS    20
#define FRAME_SAMPLES (SAMPLE_RATE * FRAME_MS / 1000)  /* 960 */

static int test_encode_decode_silence(void) {
    printf("test_encode_decode_silence: ");

    opus_codec_encoder_t* enc = opus_codec_encoder_create(SAMPLE_RATE, CHANNELS);
    if (!enc) { printf("FAIL (encoder_create)\n"); return 1; }

    opus_codec_decoder_t* dec = opus_codec_decoder_create(SAMPLE_RATE, CHANNELS);
    if (!dec) { printf("FAIL (decoder_create)\n"); opus_codec_encoder_destroy(enc); return 1; }

    int16_t pcm_in[FRAME_SAMPLES];
    memset(pcm_in, 0, sizeof(pcm_in));

    uint8_t packet[4000];
    int packet_len = opus_codec_encode(enc, pcm_in, FRAME_SAMPLES, packet, sizeof(packet));
    if (packet_len <= 0) {
        printf("FAIL (encode returned %d)\n", packet_len);
        opus_codec_encoder_destroy(enc);
        opus_codec_decoder_destroy(dec);
        return 1;
    }
    printf("encoded silence: %d bytes, ", packet_len);

    int16_t pcm_out[FRAME_SAMPLES];
    int decoded = opus_codec_decode(dec, packet, packet_len, pcm_out, FRAME_SAMPLES);
    if (decoded != FRAME_SAMPLES) {
        printf("FAIL (decode returned %d, expected %d)\n", decoded, FRAME_SAMPLES);
        opus_codec_encoder_destroy(enc);
        opus_codec_decoder_destroy(dec);
        return 1;
    }

    printf("decoded %d samples -> PASS\n", decoded);
    opus_codec_encoder_destroy(enc);
    opus_codec_decoder_destroy(dec);
    return 0;
}

static int test_encode_decode_sine(void) {
    printf("test_encode_decode_sine: ");

    opus_codec_encoder_t* enc = opus_codec_encoder_create(SAMPLE_RATE, CHANNELS);
    if (!enc) { printf("FAIL (encoder_create)\n"); return 1; }

    opus_codec_decoder_t* dec = opus_codec_decoder_create(SAMPLE_RATE, CHANNELS);
    if (!dec) { printf("FAIL (decoder_create)\n"); opus_codec_encoder_destroy(enc); return 1; }

    int ok = 1;
    int nframes = 10;

    for (int frame = 0; frame < nframes; frame++) {
        int16_t pcm_in[FRAME_SAMPLES];
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            long gi = (long)frame * FRAME_SAMPLES + i;
            pcm_in[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 440.0 * gi / SAMPLE_RATE));
        }

        uint8_t packet[4000];
        int packet_len = opus_codec_encode(enc, pcm_in, FRAME_SAMPLES, packet, sizeof(packet));
        if (packet_len <= 0) { printf("FAIL (encode frame %d returned %d)\n", frame, packet_len); ok = 0; break; }

        int16_t pcm_out[FRAME_SAMPLES * 2];
        int decoded = opus_codec_decode(dec, packet, packet_len, pcm_out, FRAME_SAMPLES);
        if (decoded <= 0) { printf("FAIL (decode frame %d returned %d)\n", frame, decoded); ok = 0; break; }

        /* verify output has energy (not all zeros) */
        if (frame >= 2) {
            int energy = 0;
            for (int i = 0; i < decoded && i < FRAME_SAMPLES * 2; i++)
                energy += abs(pcm_out[i]);
            if (energy == 0) { printf("FAIL (frame %d output is silent)\n", frame); ok = 0; break; }
        }
    }

    if (ok) printf("%d frames encoded/decoded -> PASS\n", nframes);
    opus_codec_encoder_destroy(enc);
    opus_codec_decoder_destroy(dec);
    return ok ? 0 : 1;
}

static int test_compression_ratio(void) {
    printf("test_compression_ratio: ");

    opus_codec_encoder_t* enc = opus_codec_encoder_create(SAMPLE_RATE, CHANNELS);
    if (!enc) { printf("FAIL (encoder_create)\n"); return 1; }

    int16_t pcm_in[FRAME_SAMPLES];
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        pcm_in[i] = (int16_t)(16000.0 * sin(2.0 * M_PI * 200.0 * i / SAMPLE_RATE));
    }

    uint8_t packet[4000];
    int packet_len = opus_codec_encode(enc, pcm_in, FRAME_SAMPLES, packet, sizeof(packet));
    int pcm_size = FRAME_SAMPLES * sizeof(int16_t);

    if (packet_len <= 0) {
        printf("FAIL (encode returned %d)\n", packet_len);
        opus_codec_encoder_destroy(enc);
        return 1;
    }

    if (packet_len >= pcm_size) {
        printf("FAIL (packet %d bytes >= PCM %d bytes, no compression)\n", packet_len, pcm_size);
        opus_codec_encoder_destroy(enc);
        return 1;
    }

    printf("compression: PCM %d bytes -> Opus %d bytes (%.1fx) -> PASS\n",
            pcm_size, packet_len, (double)pcm_size / packet_len);
    opus_codec_encoder_destroy(enc);
    return 0;
}

static int test_plc(void) {
    printf("test_plc: ");

    opus_codec_decoder_t* dec = opus_codec_decoder_create(SAMPLE_RATE, CHANNELS);
    if (!dec) { printf("FAIL (decoder_create)\n"); return 1; }

    int16_t pcm_out[FRAME_SAMPLES * 2];
    int decoded = opus_codec_decode(dec, NULL, 0, pcm_out, FRAME_SAMPLES);
    if (decoded <= 0) {
        printf("FAIL (PLC returned %d)\n", decoded);
        opus_codec_decoder_destroy(dec);
        return 1;
    }

    printf("PLC returned %d samples -> PASS\n", decoded);
    opus_codec_decoder_destroy(dec);
    return 0;
}

static int test_null_args(void) {
    printf("test_null_args: ");

    int r = opus_codec_encode(NULL, NULL, 0, NULL, 0);
    if (r >= 0) { printf("FAIL (expected error)\n"); return 1; }

    r = opus_codec_decode(NULL, NULL, 0, NULL, 0);
    if (r >= 0) { printf("FAIL (expected error)\n"); return 1; }

    opus_codec_encoder_destroy(NULL);
    opus_codec_decoder_destroy(NULL);

    printf("PASS\n");
    return 0;
}

static int test_default_sample_rate(void) {
    printf("test_default_sample_rate: ");

    opus_codec_encoder_t* enc = opus_codec_encoder_create(0, 0);
    if (!enc) { printf("FAIL (encoder_create)\n"); return 1; }

    opus_codec_decoder_t* dec = opus_codec_decoder_create(0, 0);
    if (!dec) { printf("FAIL (decoder_create)\n"); opus_codec_encoder_destroy(enc); return 1; }

    /* 20ms at default 48kHz = 960 samples */
    int16_t pcm[960];
    memset(pcm, 0, sizeof(pcm));
    uint8_t packet[4000];

    int encoded = opus_codec_encode(enc, pcm, 960, packet, sizeof(packet));
    if (encoded <= 0) { printf("FAIL (encode)\n"); opus_codec_encoder_destroy(enc); opus_codec_decoder_destroy(dec); return 1; }

    int decoded = opus_codec_decode(dec, packet, encoded, pcm, 960);
    if (decoded != 960) { printf("FAIL (decode returned %d)\n", decoded); opus_codec_encoder_destroy(enc); opus_codec_decoder_destroy(dec); return 1; }

    printf("PASS\n");
    opus_codec_encoder_destroy(enc);
    opus_codec_decoder_destroy(dec);
    return 0;
}

static int test_multiple_frames(void) {
    printf("test_multiple_frames: ");

    opus_codec_encoder_t* enc = opus_codec_encoder_create(SAMPLE_RATE, CHANNELS);
    opus_codec_decoder_t* dec = opus_codec_decoder_create(SAMPLE_RATE, CHANNELS);
    if (!enc || !dec) { printf("FAIL (create)\n"); opus_codec_encoder_destroy(enc); opus_codec_decoder_destroy(dec); return 1; }

    int ok = 1;
    for (int frame = 0; frame < 10; frame++) {
        int16_t pcm_in[FRAME_SAMPLES];
        double freq = 200.0 + frame * 50.0;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            pcm_in[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * freq * i / SAMPLE_RATE));
        }

        uint8_t packet[4000];
        int len = opus_codec_encode(enc, pcm_in, FRAME_SAMPLES, packet, sizeof(packet));
        if (len <= 0) { printf("FAIL (encode frame %d)\n", frame); ok = 0; break; }

        int16_t pcm_out[FRAME_SAMPLES];
        int decoded = opus_codec_decode(dec, packet, len, pcm_out, FRAME_SAMPLES);
        if (decoded != FRAME_SAMPLES) { printf("FAIL (decode frame %d returned %d)\n", frame, decoded); ok = 0; break; }
    }
    if (ok) printf("PASS\n");
    opus_codec_encoder_destroy(enc);
    opus_codec_decoder_destroy(dec);
    return ok ? 0 : 1;
}

/* Stereo-output плееров также принимает mono Opus; upmix выполняет сам libopus. */
static int test_mono_packet_stereo_output(void) {
    printf("test_mono_packet_stereo_output: ");
    opus_codec_encoder_t* encoder = opus_codec_encoder_create(48000, 1);
    opus_codec_decoder_t* decoder = opus_codec_decoder_create(48000, 2);
    int ok = encoder && decoder;
    if (ok) {
        int16_t input[960], output[1920]; uint8_t packet[1275];
        for (unsigned i = 0; i < 960; i++) input[i] = (int16_t)(6000 * sin(i * 0.1));
        int length = opus_codec_encode(encoder, input, 960, packet, sizeof(packet));
        ok = length > 0 && opus_codec_decode(decoder, packet, length, output, 960) == 960;
        int audible = 0;
        if (ok) for (unsigned i = 0; i < 960; i++) { audible |= output[2 * i] != 0; if (output[2 * i] != output[2 * i + 1]) ok = 0; }
        ok = ok && audible;
    }
    opus_codec_encoder_destroy(encoder); opus_codec_decoder_destroy(decoder);
    printf("%s\n", ok ? "PASS" : "FAIL"); return ok ? 0 : 1;
}

int main(void) {
    int failed = 0;
    failed += test_encode_decode_silence();
    failed += test_encode_decode_sine();
    failed += test_compression_ratio();
    failed += test_plc();
    failed += test_null_args();
    failed += test_default_sample_rate();
    failed += test_multiple_frames();
    failed += test_mono_packet_stereo_output();

    printf("\n%s: %d/%d tests passed\n", failed ? "FAIL" : "OK", 8 - failed, 8);
    return failed ? 1 : 0;
}
