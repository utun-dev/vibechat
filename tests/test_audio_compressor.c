#define _DEFAULT_SOURCE
#include "audio_compressor.h"
#include "opus_codec.h"
#include "debug_config.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static int tests_run = 0, tests_passed = 0;

#define TEST(expr, msg) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* ── helpers ── */

static void fill_sine(int16_t* buf, size_t n, int freq, int sample_rate) {
    for (size_t i = 0; i < n; i++)
        buf[i] = (int16_t)(14000.0f * sinf(2.0f * (float)M_PI * (float)freq * (float)i / (float)sample_rate));
}

static void fill_silence(int16_t* buf, size_t n) {
    memset(buf, 0, n * sizeof(int16_t));
}

/* ── tests ── */

static void test_create_destroy(void) {
    struct audio_compressor* ac = audio_compressor_create();
    TEST(ac != NULL, "create");
    audio_compressor_configure(ac, &(audio_compressor_config_t){0});
    audio_compressor_reset(ac);
    audio_compressor_destroy(ac);
    /* check no leaks via u_get_allocated_count */
    size_t unfreed = u_get_allocated_count();
    TEST(unfreed == 0, "no leaks after destroy");
    if (unfreed > 0) {
        fprintf(stderr, "  unfreed blocks: %zu\n", unfreed);
        u_report_unfreed_blocks();
    }
}

static void test_disabled_passthrough(void) {
    struct audio_compressor* ac = audio_compressor_create();
    audio_compressor_configure(ac, &(audio_compressor_config_t){0});
    audio_compressor_set_enabled(ac, 0);

    int16_t in[960]; fill_sine(in, 960, 440, 48000);
    audio_compressor_push(ac, in, 960);

    const int16_t* out = audio_compressor_output(ac);
    size_t sz = audio_compressor_output_size(ac);
    TEST(sz == 960, "passthrough size");
    int match = 1;
    for (size_t i = 0; i < sz; i++) { if (out[i] != in[i]) { match = 0; break; } }
    TEST(match, "passthrough data unchanged");

    audio_compressor_destroy(ac);
}

static void test_basic_push_flush(void) {
    struct audio_compressor* ac = audio_compressor_create();
    audio_compressor_config_t cfg = {0};
    cfg.sample_rate = 48000; cfg.channels = 1;
    cfg.block_duration_ms = 20; cfg.lookback_ms = 200; cfg.lookahead_ms = 100;
    cfg.max_gain_db = 30.0f; cfg.rise_rate_per_sec = 4.0f; cfg.target_level = 1.0f;
    audio_compressor_configure(ac, &cfg);

    int16_t in[960];
    /* push 20 blocks (400ms) — enough to fill lookback+lookahead and flush */
    for (int b = 0; b < 20; b++) {
        fill_sine(in, 960, 440, 48000);
        audio_compressor_push(ac, in, 960);
    }
    audio_compressor_flush(ac);

    const int16_t* out = audio_compressor_output(ac);
    size_t sz = audio_compressor_output_size(ac);
    TEST(sz > 0, "output not empty");
    TEST(sz >= 960 * 10, "output has reasonable size");

    /* check not all silence */
    float sum_sq = 0.0f;
    for (size_t i = 0; i < sz; i++) { float v = (float)out[i] / 32768.0f; sum_sq += v * v; }
    float rms = sqrtf(sum_sq / (float)sz);
    TEST(rms > 0.001f, "output not silence");

    audio_compressor_destroy(ac);
}

static void test_silence_in_silence_out(void) {
    struct audio_compressor* ac = audio_compressor_create();
    audio_compressor_config_t cfg = {0};
    cfg.sample_rate = 48000; cfg.channels = 1;
    cfg.block_duration_ms = 20; cfg.lookback_ms = 200; cfg.lookahead_ms = 100;
    cfg.max_gain_db = 30.0f; cfg.rise_rate_per_sec = 4.0f; cfg.target_level = 1.0f;
    audio_compressor_configure(ac, &cfg);

    int16_t in[960]; fill_silence(in, 960);
    for (int b = 0; b < 20; b++)
        audio_compressor_push(ac, in, 960);
    audio_compressor_flush(ac);

    const int16_t* out = audio_compressor_output(ac);
    size_t sz = audio_compressor_output_size(ac);
    TEST(sz > 0, "output not empty (silence)");
    /* silence should still be silence (envelope ≈ 0.0001, gain limited) */
    float sum_sq = 0.0f;
    for (size_t i = 0; i < sz; i++) { float v = (float)out[i] / 32768.0f; sum_sq += v * v; }
    float rms = sqrtf(sum_sq / (float)sz);
    TEST(rms < 0.01f, "silence stays near zero");
    audio_compressor_destroy(ac);
}

static void test_reset(void) {
    struct audio_compressor* ac = audio_compressor_create();
    audio_compressor_config_t cfg = {0};
    cfg.sample_rate = 48000; cfg.channels = 1;
    cfg.block_duration_ms = 20; cfg.lookback_ms = 200; cfg.lookahead_ms = 100;
    cfg.max_gain_db = 30.0f; cfg.rise_rate_per_sec = 4.0f; cfg.target_level = 1.0f;
    audio_compressor_configure(ac, &cfg);

    int16_t in[960];
    fill_sine(in, 960, 440, 48000);
    for (int b = 0; b < 15; b++) audio_compressor_push(ac, in, 960);
    audio_compressor_flush(ac);

    audio_compressor_reset(ac);

    fill_sine(in, 960, 880, 48000);
    for (int b = 0; b < 15; b++) audio_compressor_push(ac, in, 960);
    audio_compressor_flush(ac);

    const int16_t* out = audio_compressor_output(ac);
    size_t sz = audio_compressor_output_size(ac);
    TEST(sz > 960 * 10, "output after reset has data");

    float sum_sq = 0.0f;
    for (size_t i = 0; i < sz; i++) { float v = (float)out[i] / 32768.0f; sum_sq += v * v; }
    float rms = sqrtf(sum_sq / (float)sz);
    TEST(rms > 0.001f, "output after reset not silence");

    audio_compressor_destroy(ac);
}

static void test_stress(void) {
    srand((unsigned)time(NULL));
    for (int cycle = 0; cycle < 200; cycle++) {
        struct audio_compressor* ac = audio_compressor_create();
        audio_compressor_config_t cfg = {0};
        cfg.sample_rate = 48000; cfg.channels = 1;
        cfg.block_duration_ms = 20; cfg.lookback_ms = (rand() % 200) + 50;
        cfg.lookahead_ms = (rand() % 100) + 20;
        cfg.max_gain_db = (float)(rand() % 31);
        cfg.rise_rate_per_sec = 0.4f + (float)(rand() % 27) / 10.0f;
        cfg.target_level = 0.1f + (float)(rand() % 30) / 100.0f;
        audio_compressor_configure(ac, &cfg);

        int16_t in[960];
        int blocks = (rand() % 30) + 5;
        for (int b = 0; b < blocks; b++) {
            for (int i = 0; i < 960; i++)
                in[i] = (int16_t)((rand() % 28000) - 14000);
            audio_compressor_push(ac, in, 960);
        }
        audio_compressor_flush(ac);

        audio_compressor_reset(ac);
        blocks = (rand() % 10) + 1;
        for (int b = 0; b < blocks; b++) {
            for (int i = 0; i < 960; i++)
                in[i] = (int16_t)((rand() % 2000) - 1000);
            audio_compressor_push(ac, in, 960);
        }
        audio_compressor_flush(ac);

        const int16_t* out = audio_compressor_output(ac);
        size_t sz = audio_compressor_output_size(ac);
        TEST(sz > 0, "stress output not empty");

        audio_compressor_destroy(ac);

        size_t unfreed = u_get_allocated_count();
        TEST(unfreed == 0, "stress no leaks");
        if (unfreed > 0) break;
    }
}

/* Zero lookahead должен возвращать текущий кадр, а streaming — не выделять память за сеанс. */
static void test_zero_lookahead(void) {
    struct audio_compressor* ac = audio_compressor_create();
    audio_compressor_config_t cfg = {0};
    cfg.lookahead_ms = 0; cfg.target_level = 1.0f;
    TEST(audio_compressor_configure(ac, &cfg) == 0, "zero lookahead configured");
    int16_t input[960], output[960];
    for (int frame = 1; frame <= 20; ++frame) {
        for (int i = 0; i < 960; ++i) input[i] = (int16_t)(frame * 100);
        TEST(audio_compressor_push(ac, input, 960) == 0, "push current frame");
        TEST(audio_compressor_output_size(ac) == 960, "no hidden lookahead delay");
        TEST(memcmp(input, audio_compressor_output(ac), sizeof(input)) == 0, "no repeated earlier frames");
        audio_compressor_clear_output(ac);
    }
    audio_compressor_reset(ac);
    size_t allocations = u_get_allocated_count();
    int good = 1;
    for (int frame = 0; frame < 10000; ++frame) {
        for (int i = 0; i < 960; ++i) input[i] = (int16_t)(100 + frame);
        if (audio_compressor_process_frame(ac, input, output, 960) != 0 || memcmp(input, output, sizeof(input))) good = 0;
    }
    TEST(good, "long streaming keeps frame identity");
    TEST(u_get_allocated_count() == allocations, "streaming uses preallocated buffers");
    TEST(audio_compressor_output_size(ac) == 0, "streaming does not accumulate recording output");
    audio_compressor_destroy(ac);
    TEST(u_get_allocated_count() == 0, "streaming no leaks");
}

/* Одинаковое усиление в двух API и баланс стерео при многократном обороте истории. */
static void test_streaming_matches_recording(void) {
    struct audio_compressor* recording = audio_compressor_create();
    struct audio_compressor* streaming = audio_compressor_create();
    audio_compressor_config_t cfg = {0};
    cfg.channels = 2; cfg.max_gain_db = 15; cfg.target_level = 0.25f;
    TEST(audio_compressor_configure(recording, &cfg) == 0, "stereo recording configured");
    TEST(audio_compressor_configure(streaming, &cfg) == 0, "stereo streaming configured");
    int16_t input[1920], output[1920];
    int equal = 1;
    for (int frame = 0; frame < 500; ++frame) {
        for (int i = 0; i < 960; ++i) {
            input[2*i] = (int16_t)(sin((double)(frame * 960 + i) * 0.2) * (1000 + frame * 30));
            input[2*i+1] = -input[2*i];
        }
        if (audio_compressor_push(recording, input, 1920) != 0 ||
            audio_compressor_process_frame(streaming, input, output, 1920) != 0 ||
            audio_compressor_output_size(recording) != 1920 ||
            memcmp(output, audio_compressor_output(recording), sizeof(output))) equal = 0;
        audio_compressor_clear_output(recording);
    }
    TEST(equal, "streaming and recording share gain algorithm after ring wrap");
    audio_compressor_destroy(recording); audio_compressor_destroy(streaming);
}

/* Тракт трансляции: общий gain стереопары перед Opus, live bypass и смена предела gain. */
static void test_live_opus_gain(void) {
    struct audio_compressor* ac = audio_compressor_create();
    opus_codec_encoder_t* encoder = opus_codec_encoder_create(48000, 2);
    opus_codec_decoder_t* decoder = opus_codec_decoder_create(48000, 2);
    TEST(ac && encoder && decoder, "live audio codecs created");
    if (!ac || !encoder || !decoder) goto done;
    audio_compressor_config_t cfg = {.sample_rate = 48000, .channels = 2, .block_duration_ms = 20,
        .lookback_ms = 200, .lookahead_ms = 0, .max_gain_db = 6, .rise_rate_per_sec = 6,
        .release_rate_per_sec = 30, .target_level = 0.25f};
    TEST(audio_compressor_configure(ac, &cfg) == 0, "live radio compressor configuration");
    TEST(opus_codec_encoder_application_set(encoder, OPUS_CODEC_APP_AUDIO) == 0, "live Opus audio mode");
    TEST(opus_codec_encoder_bitrate_set(encoder, 128000) == 0, "live Opus bitrate");
    int16_t input[1920], compressed[1920], decoded[1920]; uint8_t packet[1275];
    size_t allocations = u_get_allocated_count();
    for (int frame = 0; frame < 30; frame++) {
        for (unsigned i = 0; i < 960; i++) {
            input[2 * i] = (int16_t)(500 * sin((frame * 960 + i) * 2 * M_PI * 440 / 48000));
            input[2 * i + 1] = (int16_t)(125 * sin((frame * 960 + i) * 2 * M_PI * 1100 / 48000));
        }
        TEST(audio_compressor_process_frame(ac, input, compressed, 1920) == 0, "live current stereo frame compressed");
        int length = opus_codec_encode(encoder, compressed, 960, packet, sizeof(packet));
        TEST(length > 0, "live compressed frame encoded");
        if (length <= 0) goto done;
        TEST(opus_codec_decode(decoder, packet, length, decoded, 960) == 960, "live decoded packet has 20ms of PCM");
    }
    double input_energy = 0, output_energy = 0;
    for (unsigned i = 0; i < 1920; i++) { input_energy += (double)input[i] * input[i]; output_energy += (double)decoded[i] * decoded[i]; }
    TEST(output_energy > input_energy * 2.5 && output_energy < input_energy * 5, "gain survives real Opus roundtrip");
    double left_energy = 0, right_energy = 0;
    for (unsigned i = 0; i < 960; i++) {
        left_energy += (double)decoded[2 * i] * decoded[2 * i];
        right_energy += (double)decoded[2 * i + 1] * decoded[2 * i + 1];
    }
    TEST(left_energy > right_energy * 8 && left_energy < right_energy * 25, "stereo balance survives compressor and Opus");
    TEST(u_get_allocated_count() == allocations, "live audio does not accumulate compressor buffers");
    audio_compressor_set_enabled(ac, 0);
    memcpy(compressed, input, sizeof(input));
    TEST(audio_compressor_process_frame(ac, compressed, compressed, 1920) == 0, "live bypass in place");
    TEST(memcmp(input, compressed, sizeof(input)) == 0, "disabled compressor preserves current PCM exactly");
    cfg.max_gain_db = 20;
    TEST(audio_compressor_configure(ac, &cfg) == 0, "live gain limit changed"); audio_compressor_set_enabled(ac, 1);
    for (unsigned frame = 0; frame < 50; frame++) TEST(audio_compressor_process_frame(ac, input, compressed, 1920) == 0, "live new gain frame");
    TEST(audio_compressor_gain_db(ac) > 19.9f && audio_compressor_gain_db(ac) <= 20.01f, "live new gain limit applied");
    TEST(audio_compressor_output_size(ac) == 0, "live gain update introduces no output backlog");
done:
    opus_codec_encoder_destroy(encoder); opus_codec_decoder_destroy(decoder); audio_compressor_destroy(ac);
    TEST(u_get_allocated_count() == 0, "live Opus and compressor no leaks");
}

/* Общая цель применяется к живому и новому компрессору; служебный AGC остаётся независимым. */
static void test_shared_target(void) {
    struct audio_compressor* ac = audio_compressor_create();
    struct audio_compressor* internal = audio_compressor_create();
    audio_compressor_config_t cfg = {.sample_rate=48000, .channels=1, .block_duration_ms=20, .lookback_ms=200,
        .max_gain_db=24, .rise_rate_per_sec=6, .target_level=0.25f, .use_shared_settings=1};
    TEST(audio_compressor_set_shared_settings(-12, 0) == 0, "shared target configured");
    TEST(audio_compressor_configure(ac, &cfg) == 0, "audible compressor follows shared settings");
    cfg.use_shared_settings = 0;
    TEST(audio_compressor_configure(internal, &cfg) == 0, "internal compressor keeps independent target");
    int16_t input[960], output[960]; fill_sine(input, 960, 1000, 48000);
    int targets[] = {-12, -6, -3, 0};
    size_t allocations = u_get_allocated_count();
    for (unsigned target = 0; target < sizeof(targets)/sizeof(targets[0]); ++target) {
        float previous = audio_compressor_gain_db(ac);
        TEST(audio_compressor_set_shared_settings(targets[target], 0) == 0, "live shared target accepted");
        TEST(audio_compressor_gain_db(ac) == previous, "publishing target does not reset gain");
        for (int frame=0; frame<250; ++frame) TEST(audio_compressor_process_frame(ac, input, output, 960) == 0, "shared target frame");
        double energy=0; int peak=0;
        for (int i=0; i<960; ++i) { energy+=(double)output[i]*output[i]; if (abs(output[i])>peak) peak=abs(output[i]); }
        double level=(sqrt(energy/960)+peak/2.0)/32768;
        double db=20*log10(level);
        fprintf(stderr, "shared target=%ddBFS measured=%.3fdBFS gain=%.2fdB clips=%u\n",
                targets[target], db, audio_compressor_gain_db(ac), audio_compressor_clip_count(ac));
        TEST(fabs(db-targets[target])<0.05, "steady output follows configured envelope target");
    }
    for (int frame=0; frame<250; ++frame) audio_compressor_process_frame(internal, input, output, 960);
    double energy=0; int peak=0;
    for (int i=0; i<960; ++i) { energy+=(double)output[i]*output[i]; if (abs(output[i])>peak) peak=abs(output[i]); }
    TEST(fabs((sqrt(energy/960)+peak/2.0)/32768-0.25)<0.001, "shared target does not change internal VAD normalization");
    TEST(u_get_allocated_count()==allocations, "target updates do not allocate or reset history");
    TEST(audio_compressor_set_shared_settings(1, 0)<0, "out-of-range target rejected");
    audio_compressor_destroy(ac); audio_compressor_destroy(internal);
    audio_compressor_set_shared_settings(-3, 0);
}

#ifdef TEST_SHARED_SPEECH_PATH
/* Настоящий Silero и сохранённая речь: на тишине gain стоит, при речи растёт, PCM не задерживается. */
static void test_shared_speech(void) {
    FILE* file=fopen(TEST_SHARED_SPEECH_PATH, "rb");
    TEST(file!=NULL, "speech fixture opened");
    if (!file) return;
    int16_t speech[16384];
    size_t count=fread(speech, sizeof(int16_t), 16384, file); fclose(file);
    TEST(count==16384, "speech fixture complete");
    audio_compressor_config_t cfg={.sample_rate=48000, .channels=2, .block_duration_ms=20, .lookback_ms=200,
        .max_gain_db=24, .rise_rate_per_sec=6, .target_level=0.25f, .use_shared_settings=1};
    TEST(audio_compressor_set_shared_settings(-3, 1)==0, "speech-only mode enabled");
    struct audio_compressor* ac=audio_compressor_create();
    TEST(ac && audio_compressor_configure(ac,&cfg)==0, "speech-only compressor configured");
    if (!ac) return;
    int16_t input[1920]={0}, output[1920];
    float initial=audio_compressor_gain_db(ac), maximum=initial;
    for (int frame=0; frame<100; ++frame) TEST(audio_compressor_process_frame(ac,input,output,1920)==0,"VAD silence frame");
    TEST(audio_compressor_gain_db(ac)==initial, "real VAD prevents gain growth on silence");
    for (int frame=0; frame<51; ++frame) {
        for (int i=0; i<960; ++i) {
            input[2*i]=speech[(frame*960+i)/3]/4; input[2*i+1]=-input[2*i];
        }
        TEST(audio_compressor_process_frame(ac,input,output,1920)==0, "VAD speech frame");
        if (audio_compressor_gain_db(ac)>maximum) maximum=audio_compressor_gain_db(ac);
    }
    TEST(maximum>initial+0.5f,"real VAD permits gain growth on speech including anti-phase stereo");
    memset(input,0,sizeof(input));
    for (int frame=0; frame<100; ++frame) audio_compressor_process_frame(ac,input,output,1920);
    float held=audio_compressor_gain_db(ac);
    for (int frame=0; frame<100; ++frame) audio_compressor_process_frame(ac,input,output,1920);
    TEST(audio_compressor_gain_db(ac)==held,"real VAD freezes gain after speech ends");
    fprintf(stderr,"real shared VAD: initial=%.2fdB speech_max=%.2fdB silence_held=%.2fdB\n",initial,maximum,held);
    audio_compressor_destroy(ac); audio_compressor_set_shared_settings(-3,0);
}
#endif

int main(void) {
    debug_set_level(DEBUG_LEVEL_NONE);

    test_create_destroy();
    test_disabled_passthrough();
    test_basic_push_flush();
    test_silence_in_silence_out();
    test_reset();
    test_stress();
    test_zero_lookahead();
    test_streaming_matches_recording();
    test_live_opus_gain();
    test_shared_target();
#ifdef TEST_SHARED_SPEECH_PATH
    test_shared_speech();
#endif

    fprintf(stderr, "\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
