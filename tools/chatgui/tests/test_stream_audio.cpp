/* Реальный stereo Opus/SoundTouch: высота тонов, баланс каналов и отображение услышанных кадров в исходный PTS. */
#include "../../../tests/stream_address_fixture.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <deque>

extern "C" {
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../lib/opus_codec.h"
#include "../../../src/stream/stream_playback_audio.h"
#include "../../../src/stream/stream_audio_meter.h"
#include "../../../src/stream/stream_media_buffer.h"
}

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream audio test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

static void test_live_prefill_policy() {
    auto* encoder = opus_codec_encoder_create(48000, 2); auto* audio = stream_playback_audio_create(0, 250);
    CHECK(encoder && audio); int16_t pcm[1920] = {}, out[960]; uint8_t packet[1275]; unsigned packets = 0;
    int bytes = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(bytes > 0);
    auto fill = [&] {
        while (stream_playback_audio_needs_input(audio) && packets < 64) {
            CHECK(!stream_playback_audio_feed(audio, packets * 20000, packet, bytes)); packets++;
        }
        CHECK(!stream_playback_audio_needs_input(audio));
    };
    fill(); unsigned before = stream_playback_audio_buffered(audio);
    CHECK(!stream_playback_audio_set_buffer_ms(audio, 750)); CHECK(stream_playback_audio_buffering(audio));
    CHECK(stream_playback_audio_pull(audio, out, 480, INT64_MAX) == 0);
    CHECK(stream_playback_audio_buffered(audio) == before); fill();
    CHECK(stream_playback_audio_pull(audio, out, 480, INT64_MAX) == 480);
    uint64_t written = stream_playback_audio_written(audio);
    CHECK(!stream_playback_audio_set_buffer_ms(audio, 250) && stream_playback_audio_written(audio) == written);
    stream_playback_audio_destroy(audio); opus_codec_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] live PCM policy: 250->750->250ms preserves prepared PCM and PTS");
}

static void test_stereo_tempo() {
    auto* encoder = opus_codec_encoder_create(48000, 2);
    CHECK(encoder && opus_codec_encoder_application_set(encoder, OPUS_CODEC_APP_AUDIO) == 0);
    CHECK(opus_codec_encoder_bitrate_set(encoder, 128000) == 0);
    auto* audio = stream_playback_audio_create(0, 40); CHECK(audio);
    stream_playback_audio_set_tempo(audio, 1100);
    std::vector<int16_t> decoded;
    int16_t pcm[1920], output[1920]; uint8_t packet[1275];
    for (unsigned frame = 0; frame < 200; frame++) {
        for (unsigned i = 0; i < 960; i++) {
            double t = (frame * 960 + i) / 48000.0;
            pcm[2 * i] = (int16_t)std::lrint(10000 * std::sin(2 * 3.141592653589793 * 1000 * t));
            pcm[2 * i + 1] = (int16_t)std::lrint(4000 * std::sin(2 * 3.141592653589793 * 500 * t));
        }
        int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
        CHECK(stream_playback_audio_feed(audio, frame * 20000, packet, (size_t)length) == 0);
        for (;;) {
            int count = stream_playback_audio_pull(audio, output, 480, INT64_MAX); CHECK(count >= 0);
            if (!count) break;
            decoded.insert(decoded.end(), output, output + count * 2);
        }
    }
    stream_playback_audio_finish(audio);
    for (unsigned i = 0; !stream_playback_audio_empty(audio) && i < 100; i++) {
        int count = stream_playback_audio_pull(audio, output, 480, INT64_MAX); CHECK(count > 0);
        decoded.insert(decoded.end(), output, output + count * 2);
    }
    CHECK(stream_playback_audio_empty(audio));
    uint64_t written = stream_playback_audio_written(audio);
    CHECK(written == decoded.size() / 2 && std::abs((double)written - 192000 / 1.1) < 2);
    unsigned crossings[2] = {0, 0}; double energy[2] = {0, 0};
    unsigned first = 9600, last = (unsigned)written - 9600;
    for (unsigned i = first + 1; i < last; i++) for (unsigned ch = 0; ch < 2; ch++) {
        if (decoded[(i - 1) * 2 + ch] < 0 && decoded[i * 2 + ch] >= 0) crossings[ch]++;
        energy[ch] += (double)decoded[i * 2 + ch] * decoded[i * 2 + ch];
    }
    double seconds = (last - first) / 48000.0;
    CHECK(std::abs(crossings[0] / seconds - 1000) < 5 && std::abs(crossings[1] / seconds - 500) < 5);
    CHECK(std::sqrt(energy[0] / energy[1]) > 2.0 && std::sqrt(energy[0] / energy[1]) < 3.0);
    int64_t half = stream_playback_audio_position(audio, written / 2);
    CHECK(std::abs(half - (int64_t)(written / 2 * 1100000.0 / 48000)) < 100);
    int64_t gain = stream_playback_audio_pending_gain(audio, written / 2);
    CHECK(gain > 170000 && gain < 190000);
    stream_playback_audio_destroy(audio); opus_codec_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] stereo x1.1: unchanged tone pitch/balance, exact duration and heard PTS mapping");
}

static void test_silence_clock() {
    auto* encoder = opus_codec_encoder_create(48000, 2); auto* audio = stream_playback_audio_create(1000000, 40);
    CHECK(encoder && audio);
    int16_t pcm[1920] = {0}, out[1920]; uint8_t packet[1275];
    for (unsigned i = 0; i < 4; i++) {
        int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
        CHECK(stream_playback_audio_feed(audio, 1000000 + i * 20000, packet, (size_t)length) == 0);
    }
    int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
    CHECK(stream_playback_audio_feed(audio, INT64_MAX - 20000, packet, (size_t)length) == -2);
    CHECK(stream_playback_audio_feed(audio, 1080000, packet, (size_t)length) == 0);
    int got = stream_playback_audio_pull(audio, out, 960, INT64_MAX); CHECK(got > 0);
    uint64_t before = stream_playback_audio_written(audio);
    int64_t pts = stream_playback_audio_position(audio, before);
    stream_playback_audio_silence(audio, 9600);
    CHECK(stream_playback_audio_position(audio, before + 4800) == pts);
    CHECK(stream_playback_audio_starved(audio, before + 4800));
    CHECK(stream_playback_audio_pending_gain(audio, before + 4800) == 0);
    stream_playback_audio_destroy(audio); opus_codec_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] device silence freezes source time and preserves stereo decoder state");
}

/* Готовые пакеты ждут worker: больший запас покрывает паузу его планирования между callback устройства. */
static void test_pcm_buffer() {
    CHECK(!stream_playback_audio_create(0, 0));
    CHECK(!stream_playback_audio_create(0, 1001));
    auto* encoder = opus_codec_encoder_create(48000, 2); CHECK(encoder);
    int16_t pcm[1920] = {0}, output[960]; uint8_t packet[1275];
    int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
    for (unsigned bufferMs : {20u, 40u, 250u}) {
        auto* audio = stream_playback_audio_create(0, bufferMs); CHECK(audio);
        unsigned packets = 0;
        while (stream_playback_audio_needs_input(audio) && packets < 64) {
            CHECK(stream_playback_audio_feed(audio, packets * 20000, packet, (size_t)length) == 0); ++packets;
        }
        CHECK(!stream_playback_audio_needs_input(audio));
        unsigned played = 0;
        for (unsigned callback = 0; callback < 4; callback++) {
            int count = stream_playback_audio_pull(audio, output, 480, INT64_MAX); CHECK(count >= 0);
            played += (unsigned)count;
        }
        CHECK(bufferMs >= 40 ? played == 1920 : played < 1920);
        if (bufferMs == 250) {
            for (unsigned callback = 4; callback < 20; callback++) {
                int count = stream_playback_audio_pull(audio, output, 480, INT64_MAX); CHECK(count == 480);
                played += count;
            }
            CHECK(played == 9600);
        }
        CHECK(stream_playback_audio_needs_input(audio));
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] PCM buffer=%ums worker_gap=%ums played=%u frames",
            bufferMs, bufferMs == 250 ? 200u : 40u, played);
        stream_playback_audio_destroy(audio);
    }
    opus_codec_encoder_destroy(encoder);
}

/* Настоящие producer/consumer: callback не обращается к DSP даже при x1.6 и частичных блоках. */
static void test_pcm_concurrent() {
    auto* encoder = opus_codec_encoder_create(48000, 2); CHECK(encoder);
    int16_t pcm[1920] = {}; uint8_t packet[1275];
    int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
    auto* audio = stream_playback_audio_create(0, 250); CHECK(audio);
    stream_playback_audio_set_tempo(audio, 1600);
    std::thread producer([&] {
        for (unsigned i = 0; i < 400; ++i) {
            while (!stream_playback_audio_needs_input(audio)) std::this_thread::yield();
            CHECK(stream_playback_audio_feed(audio, i * 20000, packet, length) == 0);
        }
        stream_playback_audio_finish(audio);
    });
    int16_t output[777 * 2]; uint64_t frames = 0; int64_t previous = 0;
    while (!stream_playback_audio_empty(audio)) {
        int count = stream_playback_audio_pull(audio, output, 777, INT64_MAX); CHECK(count >= 0);
        frames += count;
        int64_t position = stream_playback_audio_position(audio, frames);
        CHECK(position >= previous); previous = position;
        if (!count) std::this_thread::yield();
    }
    producer.join();
    CHECK(frames == 240000 && previous == 8000000);
    stream_playback_audio_destroy(audio); opus_codec_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] independent PCM producer/consumer: stereo x1.6, 777-frame callbacks, exact EOF/PTS");
}

/* Детерминированные часы: 33мс после KEY, пачки по 100мс, джиттер 95..106мс и ещё 20мс до feed.
 * Это исходный desktop-флак без QTimer/sleep: после первого звука ни один callback не теряет PCM. */
static void test_bursty_pcm_delivery() {
    auto* encoder = opus_codec_encoder_create(48000, 2); CHECK(encoder);
    auto* audio = stream_playback_audio_create(966666, 250); CHECK(audio);
    auto* meter = stream_audio_meter_create(); CHECK(meter);
    stream_playback_audio_set_meter(audio, stream_audio_meter_retain(meter));
    stream_audio_meter_ready(meter, 1000000);
    int16_t pcm[1920] = {}, output[960]; uint8_t packet[1275];
    int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
    CHECK(stream_playback_audio_feed(audio, 960000, packet, length) == 0);
    CHECK(stream_playback_audio_feed(audio, 980000, packet, length) == 0);
    struct Batch { int64_t at, pts; };
    std::deque<Batch> pending;
    const int jitter[] = {0, 6000, 1000, 6000, 1000};
    for (unsigned i = 0; i < 80; ++i) pending.push_back({100000 + i * 100000 + jitter[i % 5] + 20000, 1000000 + i * 100000});
    bool started = false; unsigned callbacks = 0, silent = 0;
    for (int64_t now = 0; now < 8000000; now += 10000) {
        while (!pending.empty() && pending.front().at <= now) {
            stream_audio_meter_ready(meter, pending.front().pts + 100000);
            for (unsigned i = 0; i < 5; ++i)
                CHECK(stream_playback_audio_feed(audio, pending.front().pts + i * 20000, packet, length) == 0);
            pending.pop_front();
        }
        int count = stream_playback_audio_pull(audio, output, 480, INT64_MAX); CHECK(count >= 0);
        if (started || count) { CHECK(count == 480); started = true; ++callbacks; }
        else { ++silent; CHECK(stream_playback_audio_position(audio, stream_playback_audio_written(audio)) == 966666); }
        if (count < 480) stream_playback_audio_silence(audio, 480 - count);
        CHECK(stream_playback_audio_position(audio, stream_playback_audio_written(audio)) >= 966666);
    }
    CHECK(started && callbacks > 700 && silent < 40);
    stream_stat_sample stats{};
    stream_audio_meter_collect(meter, &stats, 1);
    CHECK(stats.v[ST_AUDIO_OBSERVED] == callbacks * 10 && !stats.v[ST_PAUSES] && !stats.v[ST_PAUSE_MS] && !stats.v[ST_PAUSE_MAX]);
    stream_audio_meter_release(meter);
    stream_playback_audio_destroy(audio); opus_codec_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] 100ms bursts + 95..106ms jitter + 20ms worker delay: callbacks=%u no PCM gaps", callbacks);
}

/* Длинная пауза не дробит звук на отдельные пришедшие пакеты; EOF выпускает короткий остаток без ожидания. */
static void test_pcm_recovery_and_short_eof() {
    auto* encoder = opus_codec_encoder_create(48000, 2); CHECK(encoder);
    int16_t pcm[1920] = {}, output[960]; uint8_t packet[1275];
    int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
    auto* audio = stream_playback_audio_create(1000000, 250); CHECK(audio);
    auto* meter = stream_audio_meter_create(); CHECK(meter);
    stream_playback_audio_set_meter(audio, stream_audio_meter_retain(meter));
    stream_audio_meter_ready(meter, 1000000);
    CHECK(!stream_playback_audio_pull(audio, output, 480, INT64_MAX));
    stream_stat_sample stats{};
    stream_audio_meter_collect(meter, &stats, 1);
    CHECK(!stats.v[ST_AUDIO_OBSERVED] && !stats.v[ST_PAUSES]);
    int64_t pts = 1000000;
    while (stream_playback_audio_needs_input(audio)) {
        CHECK(stream_playback_audio_feed(audio, pts, packet, length) == 0); pts += 20000;
    }
    stream_audio_meter_ready(meter, pts);
    unsigned prepared = stream_playback_audio_buffered(audio), consumed = 0;
    for (unsigned callback = 0; callback < 60; ++callback) {
        int count = stream_playback_audio_pull(audio, output, 480, INT64_MAX); CHECK(count >= 0);
        consumed += count;
        if (count < 480) stream_playback_audio_silence(audio, 480 - count);
    }
    CHECK(consumed == prepared);
    CHECK(stream_playback_audio_buffering(audio));
    stream_audio_meter_collect(meter, &stats, 1);
    CHECK(stats.v[ST_PAUSES] == 1 && stats.v[ST_PAUSED] && stats.v[ST_AUDIO_OBSERVED] == 600);
    CHECK(stats.v[ST_PAUSE_MS] == (60 * 480 - prepared + 47) / 48 && stats.v[ST_PAUSE_MAX] == stats.v[ST_PAUSE_MS]);
    uint64_t gapMs = stats.v[ST_PAUSE_MS];
    int64_t held = stream_playback_audio_position(audio, stream_playback_audio_written(audio));
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(stream_playback_audio_feed(audio, pts, packet, length) == 0); pts += 20000;
        CHECK(stream_playback_audio_pull(audio, output, 480, INT64_MAX) == 0);
        stream_playback_audio_silence(audio, 480);
        CHECK(stream_playback_audio_position(audio, stream_playback_audio_written(audio)) == held);
    }
    while (stream_playback_audio_needs_input(audio)) {
        CHECK(stream_playback_audio_feed(audio, pts, packet, length) == 0); pts += 20000;
    }
    CHECK(stream_playback_audio_pull(audio, output, 480, INT64_MAX) == 480); consumed += 480;
    CHECK(!stream_playback_audio_buffering(audio));
    stats = {};
    stream_audio_meter_collect(meter, &stats, 1);
    CHECK(!stats.v[ST_PAUSES] && !stats.v[ST_PAUSED] && stats.v[ST_PAUSE_MS] == 50 && stats.v[ST_PAUSE_MAX] == gapMs + 50);
    /* Следующие пакеты уже есть у reader: медленный локальный decoder не является сетевым глитчем. */
    stream_audio_meter_ready(meter, pts + 1000000);
    int64_t resumed = stream_playback_audio_position(audio, stream_playback_audio_written(audio)); CHECK(resumed > held);
    int limited = stream_playback_audio_pull(audio, output, 480, resumed + 1000);
    CHECK(limited > 0 && limited < 480 && !stream_playback_audio_buffering(audio)); consumed += limited;
    for (unsigned i = 0; i < 100; ++i) {
        int count = stream_playback_audio_pull(audio, output, 480, INT64_MAX); CHECK(count >= 0); consumed += count;
        if (count < 480) { stream_playback_audio_silence(audio, 480 - count); break; }
    }
    CHECK(stream_playback_audio_buffering(audio));
    CHECK(stream_playback_audio_feed(audio, pts, packet, length) == 0); pts += 20000;
    CHECK(stream_playback_audio_pull(audio, output, 480, INT64_MAX) == 0);
    stream_playback_audio_finish(audio);
    for (unsigned i = 0; !stream_playback_audio_empty(audio) && i < 100; ++i) {
        int count = stream_playback_audio_pull(audio, output, 480, INT64_MAX); CHECK(count > 0); consumed += count;
    }
    CHECK(stream_playback_audio_empty(audio) && consumed == (pts - 1000000) * 48000 / 1000000);
    CHECK(stream_playback_audio_position(audio, stream_playback_audio_written(audio)) == pts);
    stats = {};
    stream_audio_meter_collect(meter, &stats, 1);
    CHECK(!stats.v[ST_PAUSES] && !stats.v[ST_PAUSE_MS] && !stats.v[ST_PAUSED]);
    stream_audio_meter_release(meter);
    stream_playback_audio_destroy(audio);
    audio = stream_playback_audio_create(0, 250); CHECK(audio);
    CHECK(stream_playback_audio_feed(audio, 0, packet, length) == 0); stream_playback_audio_finish(audio);
    CHECK(stream_playback_audio_pull(audio, output, 480, INT64_MAX) == 480);
    CHECK(stream_playback_audio_pull(audio, output, 480, INT64_MAX) == 480);
    CHECK(stream_playback_audio_empty(audio) && stream_playback_audio_position(audio, 960) == 20000);
    stream_playback_audio_destroy(audio); opus_codec_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM,
        "[PASS] 600ms gap: trickle waits, PTS freezes/resumes, PTS limit keeps state; recovery tail and 20ms EOF drain exactly");
}

/* Новый темп не меняет PTS уже приготовленного PCM; дробный конец не теряется при сменах темпа. */
static void test_prepared_tempo_changes() {
    auto* encoder = opus_codec_encoder_create(48000, 2); CHECK(encoder);
    auto* audio = stream_playback_audio_create(0, 250); CHECK(audio);
    int16_t pcm[1920] = {}, output[1554]; uint8_t packet[1275];
    int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
    stream_playback_audio_set_tempo(audio, 1600);
    unsigned packets = 0; double expected = 0;
    while (stream_playback_audio_needs_input(audio)) {
        CHECK(stream_playback_audio_feed(audio, packets++ * 20000, packet, length) == 0); expected += 600;
    }
    int64_t pending = stream_playback_audio_pending_gain(audio, 0);
    CHECK(pending > 140000);
    stream_playback_audio_set_tempo(audio, 1000);
    CHECK(stream_playback_audio_pending_gain(audio, 0) == pending);
    CHECK(stream_playback_audio_pull(audio, output, 480, INT64_MAX) == 480);
    CHECK(stream_playback_audio_position(audio, 480) == 16000);
    uint64_t heard = 480;
    int64_t previous = 16000;
    const int tempos[] = {1000, 1001, 1100, 1250, 1457, 1555, 1600};
    for (unsigned turn = 0; packets < 400 || !stream_playback_audio_empty(audio); ++turn) {
        CHECK(turn < 10000);
        if (packets < 400 && stream_playback_audio_needs_input(audio)) {
            int tempo = tempos[packets % 7]; stream_playback_audio_set_tempo(audio, tempo);
            CHECK(stream_playback_audio_feed(audio, packets++ * 20000, packet, length) == 0);
            expected += 960000.0 / tempo;
            if (packets == 400) stream_playback_audio_finish(audio);
        }
        CHECK(stream_playback_audio_prepare(audio) == 0);
        int count = stream_playback_audio_pull(audio, output, 777, INT64_MAX); CHECK(count >= 0);
        heard += count;
        int64_t position = stream_playback_audio_position(audio, heard);
        CHECK(position >= previous && position <= 8000000); previous = position;
        CHECK(stream_playback_audio_pending_gain(audio, heard) >= 0);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "variable tempo EOF frames=%llu expected=%.6f source_pts=%lld pending_gain=%lld",
        (unsigned long long)heard, expected, (long long)previous, (long long)stream_playback_audio_pending_gain(audio, heard));
    CHECK(heard == (uint64_t)std::llround(expected) && previous == 8000000);
    CHECK(stream_playback_audio_pending_gain(audio, heard) <= 1);
    stream_playback_audio_destroy(audio); opus_codec_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] prepared PCM preserves old tempo; variable x1.0..1.6 has exact duration/EOF and monotonic PTS");
}

/* PTS из Windows→Android лога: звук начинается на 428ms позже KEY, audio clock ещё неподвижен. */
static void test_late_audio_start() {
    constexpr int64_t videoPts = 20669261, audioPts = 21097059;
    stream_descriptor descriptor{};
    descriptor.group_id = 1; descriptor.source_epoch = 3;
    descriptor.n = 8; descriptor.r = 2; descriptor.block_size = STREAM_RS_DEFAULT_SIZE;
    uint8_t secret[32] = {7}, group_seed[32] = {8};
    const auto group = stream_test_public(group_seed);
    memcpy(descriptor.group_key, group.ed, sizeof(descriptor.group_key));
    descriptor.source = stream_test_public(secret);
    stream_encoder* encoder = nullptr; stream_buffer* buffer = nullptr;
    stream_media_store* store = nullptr; stream_media_cursor* cursor = nullptr;
    CHECK(stream_encoder_create(&encoder, &descriptor, secret) == STREAM_OK);
    CHECK(stream_buffer_create(&buffer, &descriptor, stream_encoder_public_key(encoder), 256 * 1300) == STREAM_OK);
    CHECK(stream_media_store_create(&store, buffer, &descriptor) == STREAM_OK);
    CHECK(stream_media_cursor_create(&cursor, store, STREAM_MEDIA_TRACK_VIDEO | STREAM_MEDIA_TRACK_AUDIO) == STREAM_OK);
    auto* opus = opus_codec_encoder_create(48000, 2); CHECK(opus);
    int16_t pcm[1920] = {}, output[1920]; uint8_t packet[1275], video[8] = {};
    int length = opus_codec_encode(opus, pcm, 960, packet, sizeof(packet)); CHECK(length > 0);
    stream_media_block block{};
    block.format = {64, 64, 0, STREAM_MEDIA_H265, STREAM_MEDIA_OPUS_STEREO, 1}; block.video_count = 2; block.audio_count = 10;
    block.video[0] = {1, videoPts, sizeof(video), 0, sizeof(video), STREAM_MEDIA_KEY, video};
    block.video[1] = {2, audioPts + 200000, sizeof(video), 0, sizeof(video), 0, video};
    for (unsigned i = 0; i < block.audio_count; i++) block.audio[i] = {(uint64_t)(audioPts + i * 20000), (uint16_t)length, 960, packet};
    uint8_t wire[7345]; size_t wireLength = 0, partSize = 0; const uint8_t* parts[RS_MAX_BLOCKS];
    CHECK(stream_media_encode(&block, wire, sizeof(wire), &wireLength) == STREAM_OK);
    CHECK(stream_encoder_encode(encoder, 0, 1, 0, wire, wireLength, parts, &partSize) == STREAM_OK);
    for (unsigned i = 0; i < 8; i++) CHECK(stream_buffer_put(buffer, parts[i], partSize) == STREAM_OK);
    CHECK(stream_media_store_feed(store, 0, wire, wireLength) == STREAM_OK);
    for (unsigned pass = 0; pass < 2; pass++) {
        auto* audio = stream_playback_audio_create(videoPts, 40); CHECK(audio);
        stream_media_packet record{}; stream_media_status status{};
        CHECK(stream_media_cursor_status(cursor, &status) == STREAM_OK && status.start_us == videoPts);
        CHECK(stream_media_cursor_read(cursor, videoPts, 0, -1, &record) == STREAM_WAIT);
        CHECK(stream_media_cursor_read(cursor, -1, 0, 80000, &record) == STREAM_WAIT); // Preroll не включает будущий звук.
        CHECK(stream_media_cursor_read(cursor, videoPts, 1, 80000, &record) == STREAM_OK);
        CHECK(record.kind == STREAM_MEDIA_READ_VIDEO && record.video.pts_us == videoPts);
        for (unsigned i = 0; i < 5; i++) {
            CHECK(stream_media_cursor_read(cursor, videoPts, 0, 80000, &record) == STREAM_OK);
            CHECK(record.kind == STREAM_MEDIA_READ_AUDIO && record.audio.pts_us == (uint64_t)(audioPts + i * 20000));
            CHECK(stream_playback_audio_feed(audio, record.audio.pts_us, record.audio.data, record.audio.length) == 0);
        }
        CHECK(stream_media_cursor_read(cursor, videoPts, 0, 80000, &record) == STREAM_WAIT); // Окно не ползёт вперёд без часов.
        CHECK(!stream_playback_audio_needs_input(audio));
        int count = stream_playback_audio_pull(audio, output, 960, status.ready_us); CHECK(count == 960);
        int64_t heard = stream_playback_audio_position(audio, count); CHECK(heard == audioPts + 20000);
        CHECK(stream_media_cursor_read(cursor, heard, 0, 80000, &record) == STREAM_OK);
        CHECK(record.kind == STREAM_MEDIA_READ_AUDIO && record.audio.pts_us == audioPts + 100000);
        stream_playback_audio_destroy(audio); stream_media_cursor_reset(cursor);
    }
    opus_codec_encoder_destroy(opus); stream_media_cursor_destroy(cursor); stream_media_store_destroy(store);
    stream_buffer_destroy(buffer); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] late audio: 428ms startup gap, bounded 80ms priming, real Opus/SoundTouch clock advances and reset repeats");
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    size_t allocations = u_get_allocated_count();
    test_bursty_pcm_delivery(); test_pcm_recovery_and_short_eof();
    test_live_prefill_policy(); test_stereo_tempo(); test_silence_clock(); test_pcm_buffer(); test_pcm_concurrent(); test_prepared_tempo_changes(); test_late_audio_start();
    CHECK(u_get_allocated_count() == allocations);
    return 0;
}
