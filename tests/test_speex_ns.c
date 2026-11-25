/* Настоящий Speex: нулевая задержка обхода, подавление шума, независимость каналов и хвост записи. */
#include "../lib/speex_ns.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct output { int count; int16_t pcm[16000]; };

static int collect(void* arg, const int16_t* pcm, int count) {
    struct output* out = arg;
    assert(out->count + count <= (int)(sizeof(out->pcm) / sizeof(out->pcm[0])));
    memcpy(out->pcm + out->count, pcm, (size_t)count * sizeof(*pcm)); out->count += count;
    return 0;
}

static int reject(void* arg, const int16_t* pcm, int count) { (void)arg; (void)pcm; (void)count; return -1; }

/* Выключение не накапливает даже единственный отсчёт и оставляет данные побайтно прежними. */
static void bypass(void) {
    int16_t pcm[] = {12345, -23456, 32767, -32768, 1}, original[5];
    memcpy(original, pcm, sizeof(pcm));
    assert(!speex_ns_process_frame(NULL, pcm, 5) && !memcmp(pcm, original, sizeof(pcm)));
    struct output out = {0};
    for (int i = 0; i < 5; ++i) {
        assert(!speex_ns_push(NULL, pcm + i, 1, collect, &out));
        assert(out.count == i + 1 && out.pcm[i] == original[i]);
    }
    assert(!speex_ns_finish(NULL, collect, &out) && out.count == 5);
    assert(speex_ns_push(NULL, pcm, 5, reject, NULL) == -1);
    assert(!speex_ns_create(0, -20) && !speex_ns_create(3, -20) && !speex_ns_create(1, 0));
    assert(!speex_ns_create(1, -41) && !speex_ns_create(1, -100));
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS speex-ns: bypass is immediate and bit-exact; invalid settings rejected");
}

/* Mono и левый stereo канал должны быть идентичны; шум не протекает в нулевой правый канал. */
static void noise_and_stereo(void) {
    struct speex_ns* mono = speex_ns_create(1, -20);
    struct speex_ns* stereo = speex_ns_create(2, -20);
    assert(mono && stereo);
    uint32_t rng = 1234567;
    double input = 0, output = 0;
    for (int frame = 0; frame < 300; ++frame) {
        int16_t a[960], b[1920];
        for (int i = 0; i < 960; ++i) {
            rng = rng * 1664525u + 1013904223u;
            a[i] = ((int)(rng >> 16) - 32768) / 8; b[2 * i] = a[i]; b[2 * i + 1] = 0;
            if (frame >= 200) input += (double)a[i] * a[i];
        }
        assert(!speex_ns_process_frame(mono, a, 960) && !speex_ns_process_frame(stereo, b, 1920));
        for (int i = 0; i < 960; ++i) {
            assert(a[i] == b[2 * i] && b[2 * i + 1] == 0);
            if (frame >= 200) output += (double)a[i] * a[i];
        }
    }
    assert(output < input * 0.1);
    assert(!speex_ns_set_level(mono, -40));
    assert(speex_ns_set_level(mono, 0) == -1);
    int16_t invalid[960] = {0}; assert(speex_ns_process_frame(stereo, invalid, 960) == -1);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS speex-ns: stationary noise=%.2fdB, stereo channels independent", 10 * log10(output / input));
    speex_ns_destroy(mono); speex_ns_destroy(stereo);
}

/* Произвольные границы входных чанков не меняют результат, padding не попадает в запись. */
static void recording(int channels, int frames) {
    struct speex_ns* ns = speex_ns_create(channels, -1);
    struct speex_ns* reference = speex_ns_create(channels, -1);
    assert(ns && reference);
    int16_t input[6000];
    for (int i = 0; i < frames * channels; ++i) input[i] = (i * 127 % 12000) - 6000;
    input[(frames - 1) * channels] = 24000;
    struct output actual = {0}, expected = {0};
    for (int offset = 0; offset < frames;) {
        int take = (offset * 13 % 251) + 1;
        if (take > frames - offset) take = frames - offset;
        assert(!speex_ns_push(ns, input + offset * channels, take * channels, collect, &actual));
        offset += take;
    }
    assert(!speex_ns_finish(ns, collect, &actual));
    assert(actual.count == (frames + 960) * channels);
    assert(!speex_ns_finish(ns, collect, &actual) && actual.count == (frames + 960) * channels);
    assert(speex_ns_push(ns, input, channels, collect, &actual) == -1);
    for (int offset = 0; offset < frames + 960; offset += 960) {
        int16_t pcm[1920] = {0};
        int take = frames - offset;
        if (take > 960) take = 960;
        if (take > 0) memcpy(pcm, input + offset * channels, (size_t)take * channels * sizeof(int16_t));
        assert(!speex_ns_process_frame(reference, pcm, 960 * channels));
        int emit = frames + 960 - offset;
        if (emit > 960) emit = 960;
        collect(&expected, pcm, emit * channels);
    }
    assert(actual.count == expected.count && !memcmp(actual.pcm, expected.pcm, (size_t)actual.count * sizeof(int16_t)));
    assert(abs(actual.pcm[(frames + 959) * channels]) > 1000); // Импульс конца не потерян внутри фильтра.
    speex_ns_destroy(ns); speex_ns_destroy(reference);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count();
    bypass(); noise_and_stereo();
    const int lengths[] = {1, 120, 959, 960, 961, 2345};
    for (int channels = 1; channels <= 2; ++channels)
        for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) recording(channels, lengths[i]);
    struct output out = {0}; struct speex_ns* empty = speex_ns_create(1, -20);
    assert(empty && !speex_ns_finish(empty, collect, &out) && out.count == 0); speex_ns_destroy(empty);
    assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS speex-ns: mono/stereo short recordings, arbitrary chunks, finish and lifecycle");
    return 0;
}
