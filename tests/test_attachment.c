/* Строгие метаданные и реальный Opus-файл: границы, повреждения, длительность. */
#include <stdio.h>
#include <string.h>
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/opus_codec.h"
#include "../src/media_async/attachment.h"
#include "../src/media_async/voice_file.h"
#ifdef __linux__
#include <sys/mman.h>
#endif
#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "attachment check failed line=%d\n", __LINE__); return 1; } } while (0)

/* Проверяем настоящий контейнер и число декодируемых кадров, включая отбрасывание хвоста. */
static int check_voice_roundtrip(const int16_t* pcm, size_t count, int rate, int channels, int preset, uint32_t duration_ms) {
    struct attachment_info info = { .name = "voice.opus" };
    const char* path = "test_attachment_voice.opus";
    REQUIRE(!voice_file_encode(pcm, count, rate, channels, preset, path, &info));
    REQUIRE(info.duration_ms == duration_ms && strlen(info.waveform) == 100 && !attachment_validate(&info));
    FILE* file = fopen(path, "rb"); REQUIRE(file != NULL);
    uint8_t header[12]; REQUIRE(fread(header, 1, 12, file) == 12);
    uint32_t magic, saved_rate; uint16_t saved_channels, frame;
    memcpy(&magic, header, 4); memcpy(&saved_rate, header + 4, 4);
    memcpy(&saved_channels, header + 8, 2); memcpy(&frame, header + 10, 2);
    REQUIRE(magic == 0x5355504f && saved_rate == (uint32_t)rate && saved_channels == channels && frame == rate / 50);
    opus_codec_decoder_t* decoder = opus_codec_decoder_create(rate, channels); REQUIRE(decoder != NULL);
    uint8_t packet[4000]; int16_t decoded[1920]; unsigned frames = 0;
    for (;;) {
        uint16_t n; REQUIRE(fread(&n, 2, 1, file) == 1);
        if (!n) break;
        REQUIRE(n <= sizeof(packet) && fread(packet, 1, n, file) == n);
        REQUIRE(opus_codec_decode(decoder, packet, n, decoded, frame) == frame); frames++;
    }
    REQUIRE(frames == duration_ms / 20 && fgetc(file) == EOF);
    REQUIRE(!fclose(file) && !remove(path)); opus_codec_decoder_destroy(decoder);
    return 0;
}

#ifdef __linux__
static int encoder_probe;
static unsigned encoder_calls;
opus_codec_encoder_t* __real_opus_codec_encoder_create(int rate, int channels);
opus_codec_encoder_t* __wrap_opus_codec_encoder_create(int rate, int channels) {
    if (!encoder_probe) return __real_opus_codec_encoder_create(rate, channels);
    encoder_calls++;
    return NULL; /* Останавливаем только probe: PCM ещё не прочитан, файл/метаданные не изменены. */
}

#if SIZE_MAX > UINT32_MAX
/* Виртуальный PCM позволяет проверить суточную границу без выделения физической памяти/суточного Opus. */
static int check_voice_duration(void) {
    static const int rates[] = {8000, 12000, 16000, 24000, 48000};
    static const uint32_t durations[] = {0, 479, 480, 499, 500, 519, 86399980, 86400000, 86400019, 86400020, 86400980, 86401000};
    const char* path = "test_attachment_duration.opus";
    static const char marker[] = "existing voice output";
    size_t bytes = (size_t)48000 * 2 * 86401 * sizeof(int16_t);
    int16_t* pcm = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    REQUIRE(pcm != MAP_FAILED);
    FILE* file = fopen(path, "wb"); REQUIRE(file != NULL);
    REQUIRE(fwrite(marker, 1, sizeof(marker), file) == sizeof(marker) && !fclose(file));
    encoder_probe = 1;
    unsigned cases = 0, rejected = 0;
    debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (int channels = 1; channels <= 2; channels++) {
            for (size_t d = 0; d < sizeof(durations) / sizeof(durations[0]); d++) {
                uint32_t duration = durations[d];
                size_t count = (size_t)rates[r] * channels * duration / 1000;
                int allowed = duration >= 500 && duration < 86400020;
                struct attachment_info info = { .kind = ATTACHMENT_FILE, .name = "unchanged.bin" };
                unsigned char before[sizeof(info)]; memcpy(before, &info, sizeof(info));
                encoder_calls = 0;
                REQUIRE(voice_file_encode(pcm, count, rates[r], channels, 0, path, &info) == -1);
                REQUIRE(encoder_calls == (unsigned)allowed && !memcmp(&info, before, sizeof(info)));
                file = fopen(path, "rb"); REQUIRE(file != NULL);
                char saved[sizeof(marker)]; REQUIRE(fread(saved, 1, sizeof(saved), file) == sizeof(saved));
                REQUIRE(!memcmp(saved, marker, sizeof(saved)) && fgetc(file) == EOF && !fclose(file));
                cases++; rejected += !allowed;
            }
        }
    }
    encoder_probe = 0;
    REQUIRE(!munmap(pcm, bytes) && !remove(path));
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "voice duration: cases=%u rejected_before_encoder=%u file/metadata preserved", cases, rejected);
    debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_ERROR);
    REQUIRE(cases == 120 && rejected == 70);
    return 0;
}
#endif
#endif

int main(void) {
    struct attachment_info mkv = { .kind = ATTACHMENT_VIDEO, .name = "recording.mkv" };
    REQUIRE(!strcmp(attachment_info_content_type(&mkv), "video/x-matroska"));
    snprintf(mkv.name, sizeof(mkv.name), "video.mp4"); REQUIRE(!strcmp(attachment_info_content_type(&mkv), "video/mp4"));

    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    struct attachment_info in = { .kind = ATTACHMENT_VOICE, .name = "голос.opus", .duration_ms = 1200 }, out;
    memset(in.waveform, 'A', 100);
    uint8_t wire[ATTACHMENT_WIRE_MAX + 1];
    int length = attachment_encode(&in, wire, sizeof(wire));
    REQUIRE(length > 0 && !attachment_decode(wire, length, &out));
    REQUIRE(out.kind == in.kind && out.duration_ms == 1200 && !strcmp(in.name, out.name) && !strcmp(in.waveform, out.waveform));
    for (int i = 0; i < length; i++) REQUIRE(attachment_decode(wire, i, &out) < 0);
    REQUIRE(attachment_decode(wire, length + 1, &out) < 0);
    wire[0] = 99; REQUIRE(attachment_decode(wire, length, &out) < 0);
    wire[0] = ATTACHMENT_VOICE; wire[length - 1] = '?'; REQUIRE(attachment_decode(wire, length, &out) < 0);
    in.kind = ATTACHMENT_VIDEO; in.waveform[0] = 0; in.width = 640; in.height = 480;
    length = attachment_encode(&in, wire, sizeof(wire)); REQUIRE(length > 0 && !attachment_decode(wire, length, &out));
    REQUIRE(out.width == 640 && out.height == 480);
    in.width = 0; REQUIRE(attachment_validate(&in) < 0);
    for (int kind = ATTACHMENT_JPEG; kind <= ATTACHMENT_PNG; ++kind) {
        in = (struct attachment_info){ .kind = kind, .name = "изображение.dat", .width = 1920, .height = 1080 };
        length = attachment_encode(&in, wire, sizeof(wire)); REQUIRE(length > 0 && !attachment_decode(wire, length, &out));
        REQUIRE(out.kind == kind && out.width == 1920 && out.height == 1080 && !out.duration_ms);
        REQUIRE(!strcmp(attachment_content_type(out.kind), kind == ATTACHMENT_JPEG ? "image/jpeg" : "image/png"));
        for (int n = 0; n < length; ++n) REQUIRE(attachment_decode(wire, n, &out) < 0);
        in.duration_ms = 1; REQUIRE(attachment_validate(&in) < 0); in.duration_ms = 0;
        strcpy(in.waveform, "A"); REQUIRE(attachment_validate(&in) < 0); in.waveform[0] = 0;
        in.width = 0; REQUIRE(attachment_validate(&in) < 0);
        in.width = 16385; REQUIRE(attachment_validate(&in) < 0);
        in.width = 16384; in.height = 16384; REQUIRE(!attachment_validate(&in));
        in.height = 0; REQUIRE(attachment_validate(&in) < 0);
    }
    REQUIRE(ATTACHMENT_DURATION_MAX_MS == 86400000);
    for (int kind = ATTACHMENT_VOICE; kind <= ATTACHMENT_VIDEO; kind++) {
        in = (struct attachment_info){ .kind = kind, .name = "duration.bin", .duration_ms = 86400000 };
        if (kind == ATTACHMENT_VOICE) memset(in.waveform, 'A', 100);
        else { in.width = 640; in.height = 480; }
        length = attachment_encode(&in, wire, sizeof(wire)); REQUIRE(length > 0 && !attachment_decode(wire, length, &out));
        in.duration_ms++; REQUIRE(attachment_validate(&in) < 0 && attachment_encode(&in, wire, sizeof(wire)) < 0);
        wire[6]++; REQUIRE(attachment_decode(wire, length, &out) < 0);
    }
    in = (struct attachment_info){ .kind = ATTACHMENT_FILE, .name = "../unsafe" }; REQUIRE(attachment_validate(&in) < 0);
    strcpy(in.name, "\xc0\xaf"); REQUIRE(attachment_validate(&in) < 0);
    memset(in.name, 'n', 255); in.name[255] = 0;
    REQUIRE(attachment_encode(&in, wire, 265) < 0); REQUIRE(attachment_encode(&in, wire, 266) == 266);
    int16_t* pcm = u_calloc(49920, sizeof(*pcm)); REQUIRE(pcm != NULL);
    for (unsigned i = 0; i < 49920; i++) pcm[i] = (int16_t)((i % 200) * 100 - 10000);
    REQUIRE(!check_voice_roundtrip(pcm, 48001, 48000, 1, 1, 1000));
    static const int rates[] = {8000, 12000, 16000, 24000, 48000};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (int channels = 1; channels <= 2; channels++) {
            size_t frame = (size_t)(rates[r] / 50) * channels;
            REQUIRE(!check_voice_roundtrip(pcm, frame * 26 - 1, rates[r], channels, (r + channels) % 3, 500));
        }
    }
    const char* path = "test_attachment_voice.opus";
    REQUIRE(voice_file_encode(pcm, 23999, 48000, 1, 1, path, &in) < 0);
    REQUIRE(voice_file_encode(pcm, 48000, 48000, 1, 3, path, &in) < 0);
    REQUIRE(voice_file_encode(pcm, 48000, 44100, 1, 1, path, &in) < 0);
    u_free(pcm);
#if defined(__linux__) && SIZE_MAX > UINT32_MAX
    REQUIRE(!check_voice_duration());
#endif
    printf("test_attachment: strict metadata and shared Opus encode/decode PASS\n");
    return 0;
}
