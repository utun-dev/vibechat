/* Настоящие publisher/RS/uAV2: границы Opus, продвижение видео, OPENING и FIN без зависшей головы очереди. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/opus_codec.h"
#include "../lib/libopus/include/opus.h"
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_setting.h"
#include "../src/stream/stream.h"
#include "../src/stream/stream_publisher.h"
#include "../src/stream/stream_stats.h"
#include "test_clock.h"

static const char *current_case;
#define CHECK(x) do { if (!(x)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "publisher test case=%s line=%d: %s", current_case, __LINE__, #x); exit(1); \
} } while (0)

struct scenario {
    const char *name;
    unsigned n, r, lengths[40], count;
    unsigned video_bytes, opening, stop, second_video, pframe, batches;
    uint64_t dropped; /* Маска ожидаемо отброшенных записей; не вычисляется из реализации budget. */
};
struct received {
    const struct scenario *scenario;
    opus_codec_decoder_t *decoder;
    unsigned audio, video_bytes, batches, first_video, first_audio, next_audio;
    size_t capacity;
};

#ifdef __linux__
static struct stream_publisher *no_media_timer;
static int rate_cpu = -1, delay_publication;
static unsigned viewers, viewer_logs;
static char viewer_group[65];
void *__real_uasync_set_timeout(struct UASYNC *ua, int timeout_tb, void *arg, timeout_callback_t callback, const char *name);

/* После единственного крупного Opus короткий P-кадр должен уйти в том же actor, без media timer на 5мс. */
void *__wrap_uasync_set_timeout(struct UASYNC *ua, int timeout_tb, void *arg, timeout_callback_t callback, const char *name) {
    CHECK(arg != no_media_timer || !no_media_timer || !name || strcmp(name, "publisher_media"));
    if (delay_publication && name && !strcmp(name, "publisher_media")) timeout_tb = 3000;
    return __real_uasync_set_timeout(ua, timeout_tb, arg, callback, name);
}
err_t __real_uasync_get_load(struct UASYNC *ua, struct uasync_load *load);
err_t __wrap_uasync_get_load(struct UASYNC *ua, struct uasync_load *load) {
    err_t result = __real_uasync_get_load(ua, load);
    if (rate_cpu >= 0) { load->valid = 1; load->average_percent = load->current_percent = rate_cpu; }
    return result;
}
unsigned __wrap_stream_stats_viewer_count(struct UTUN_INSTANCE *inst, uint64_t group, uint64_t epoch) {
    (void)inst; (void)group; (void)epoch;
    return viewers;
}

static void viewer_log(int level, const char *category, const char *message) {
    if (strcmp(category, "stream") || !strstr(message, "publisher viewers group=")) return;
    CHECK(level == DEBUG_LEVEL_INFO && strstr(message, viewer_group));
    CHECK(strstr(message, viewers ? "count=1 previous=0" : "count=0 previous=1"));
    viewer_logs++;
}
#endif

/* Проверка записей после настоящей RS-публикации: порядок, Opus-декодирование и непрерывность видео. */
static void receive(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)inst;
    if (event->type != STREAM_EVENT_DATA) return;
    struct received *received = arg;
    const struct scenario *s = received->scenario;
    struct stream_media_block block;
    CHECK(event->length <= received->capacity);
    CHECK(stream_media_parse(event->data, event->length, &block) == STREAM_OK);
    CHECK(block.audio_count <= 8);
    received->batches++;
    if (block.audio_count && !received->first_audio) received->first_audio = received->batches;
    if (block.video_count && !received->first_video) received->first_video = received->batches;
    for (unsigned i = 0; i < block.audio_count; i++) {
        unsigned index = received->next_audio;
        while (index < s->count && (s->dropped & (UINT64_C(1) << index))) index++;
        CHECK(index < s->count);
        CHECK(block.audio[i].pts_us == 1000000u + index * 20000u);
        CHECK(block.audio[i].length == s->lengths[index]);
        CHECK(opus_packet_get_nb_samples(block.audio[i].data, block.audio[i].length, 48000) == 960);
        int16_t pcm[1920];
        CHECK(opus_codec_decode(received->decoder, block.audio[i].data, block.audio[i].length, pcm, 960) == 960);
        received->next_audio = index + 1; received->audio++;
    }
    for (unsigned i = 0; i < block.video_count; i++) {
        CHECK(block.video[i].offset == received->video_bytes);
        CHECK(block.video[i].frame_size == s->video_bytes && block.video[i].pts_us == 1000000);
        CHECK(block.video[i].frame_id == (s->pframe ? 2 : 1));
        CHECK(block.video[i].flags == (s->pframe ? 0 : STREAM_MEDIA_KEY));
        for (unsigned j = 0; j < block.video[i].length; j++) {
            unsigned offset = block.video[i].offset + j;
            CHECK(block.video[i].data[j] == (offset < 4 ? 0 : (uint8_t)(offset * 13 + 11)));
        }
        received->video_bytes += block.video[i].length;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "publisher receive case=%s batch=%u bytes=%zu audio=%u video=%u video_total=%u",
        s->name, received->batches, event->length, block.audio_count, block.video_count, received->video_bytes);
}

/* Независимое ядро/канал для каждой FEC-конфигурации; после destroy восстанавливается счётчик allocations. */
static void check_scenario(const struct scenario *s) {
    current_case = s->name;
    size_t allocated = u_get_allocated_count();
    test_clock_init(100);
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE *inst = utun_instance_create_from_str(ua,
        "[global]\nauto_sockets=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n"
        "member_files_enabled=0\nlocal_broadcasts_enabled=0\n");
    CHECK(inst && !utun_core_start(inst) && !chat_service_start(inst));
    chat_core_create_channel_auto(inst, s->name);
    sqlite3_stmt *statement = NULL;
    CHECK(sqlite3_prepare_v2(chat_core_get_db(inst), "SELECT channel_id FROM channels LIMIT 1", -1, &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    uint64_t group = topo_groups_channel_slot(inst->topo_groups, (const char *)sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement); CHECK(group);
    char value[16]; snprintf(value, sizeof(value), "%u", s->n);
    CHECK(!chat_setting_set(inst, "stream_subblocks", value));
    snprintf(value, sizeof(value), "%u", s->r);
    CHECK(!chat_setting_set(inst, "stream_rs_redundant", value));
    struct stream_descriptor descriptor = {.group_id = group, .source = stream_address_read(inst->my_ed25519_pubkey),
        .source_epoch = 1, .n = s->n, .r = s->r, .block_size = STREAM_RS_DEFAULT_SIZE};
    struct received received = {.scenario = s, .capacity = stream_input_size(&descriptor)};
    uasync_mark_running(ua);
    struct stream_publisher *publisher = NULL;
    CHECK(!stream_publisher_create(&publisher, inst));
    struct stream_media_format format = {64, 64, 0, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1};
    CHECK(!stream_publisher_open(publisher, 1, stream_address_read(topo_group_wire_id(topo_groups_find(inst->topo_groups, group))), &format));
    struct stream_publisher_status status;
    if (s->n - s->r > STREAM_REGULAR_LIMIT) {
        test_clock_run_for(ua, 10);
        CHECK(!stream_publisher_snapshot(publisher, 1, &status));
        CHECK(status.state == STREAM_PUBLISHER_FAILED && status.reason == STREAM_CLOSE_CAPACITY);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "publisher case=%s n=%u r=%u rejected with CLOSED_CAPACITY", s->name, s->n, s->r);
        stream_publisher_destroy(publisher);
        uasync_mark_stopped(ua); utun_instance_destroy(inst); uasync_destroy(ua, 0); test_clock_finish();
        CHECK(u_get_allocated_count() == allocated);
        return;
    }
    if (!s->opening) {
        test_clock_run_for(ua, 10);
        CHECK(!stream_publisher_snapshot(publisher, 1, &status) && status.state == STREAM_PUBLISHER_LIVE);
    }
    opus_codec_encoder_t *encoder = opus_codec_encoder_create(48000, 2); CHECK(encoder);
    received.decoder = opus_codec_decoder_create(48000, 2); CHECK(received.decoder);
    CHECK(!opus_codec_encoder_application_set(encoder, OPUS_CODEC_APP_AUDIO));
    CHECK(!opus_codec_encoder_bitrate_set(encoder, 128000));
    int16_t pcm[1920] = {0}; uint8_t original[1275], packet[1275], video[6000] = {0};
    for (unsigned i = 4; i < sizeof(video); i++) video[i] = (uint8_t)(i * 13 + 11);
    int encoded = opus_codec_encode(encoder, pcm, 960, original, sizeof(original)); CHECK(encoded > 0);
    if (s->pframe) {
        CHECK(stream_publisher_feed(publisher, 1, 1, 960000, 1, video, 5, &format) == 1);
        test_clock_run_for(ua, 300);
#ifdef __linux__
        no_media_timer = publisher;
#endif
    }
    stream_set_event_callback(inst, receive, &received);
    for (unsigned i = 0; i < s->count; i++) {
        CHECK(s->lengths[i] >= (unsigned)encoded && s->lengths[i] <= sizeof(packet));
        memcpy(packet, original, encoded);
        CHECK(opus_packet_pad(packet, encoded, s->lengths[i]) == OPUS_OK);
        CHECK(stream_publisher_feed(publisher, 1, 0, 1000000u + i * 20000u, 0, packet, s->lengths[i], NULL) == 1);
    }
    CHECK(s->video_bytes <= sizeof(video));
    if (s->video_bytes) CHECK(stream_publisher_feed(publisher, 1, 1, 1000000, !s->pframe, video, s->video_bytes, &format) == 1);
    if (s->stop) stream_publisher_stop(publisher, 1);
    test_clock_run_for(ua, 2000); /* До TTL очереди: потеря или задержка до expiry не считается успехом. */
    CHECK(!stream_publisher_snapshot(publisher, 1, &status));
    unsigned expected_audio = s->count;
    for (unsigned i = 0; i < s->count; i++) if (s->dropped & (UINT64_C(1) << i)) expected_audio--;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "publisher case=%s audio=%u/%u video=%u/%u batches=%u first_audio=%u first_video=%u state=%d",
        s->name, received.audio, expected_audio, received.video_bytes, s->video_bytes,
        received.batches, received.first_audio, received.first_video, status.state);
    CHECK(received.audio == expected_audio && received.video_bytes == s->video_bytes);
    if (s->second_video) CHECK(received.first_audio == 1 && received.first_video == 2);
    if (s->batches) CHECK(received.batches == s->batches);
    CHECK(status.state == (s->stop ? STREAM_PUBLISHER_DRAINING : STREAM_PUBLISHER_LIVE));
#ifdef __linux__
    no_media_timer = NULL;
#endif
    stream_set_event_callback(inst, NULL, NULL); stream_publisher_destroy(publisher);
    opus_codec_encoder_destroy(encoder); opus_codec_decoder_destroy(received.decoder);
    uasync_mark_stopped(ua); utun_instance_destroy(inst); uasync_destroy(ua, 0); test_clock_finish();
    CHECK(u_get_allocated_count() == allocated);
}

#ifdef __linux__
/* Реальные записи просрочиваются в actor; CPU и задержка его media timer задаются независимо. */
static void check_rate(void) {
    current_case = "source-rate";
    size_t allocated = u_get_allocated_count();
    test_clock_init(100);
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE *inst = utun_instance_create_from_str(ua,
        "[global]\nauto_sockets=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n"
        "member_files_enabled=0\nlocal_broadcasts_enabled=0\n");
    CHECK(inst && !utun_core_start(inst) && !chat_service_start(inst));
    chat_core_create_channel_auto(inst, "rate");
    sqlite3_stmt *statement = NULL;
    CHECK(sqlite3_prepare_v2(chat_core_get_db(inst), "SELECT channel_id FROM channels LIMIT 1", -1, &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    uint64_t group = topo_groups_channel_slot(inst->topo_groups, (const char *)sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement); CHECK(group);
    CHECK(!chat_setting_set(inst, "stream_subblocks", "1") && !chat_setting_set(inst, "stream_rs_redundant", "0"));
    uasync_mark_running(ua);
    struct stream_publisher *p = NULL; CHECK(!stream_publisher_create(&p, inst));
    struct stream_media_format format = {64, 64, 0, STREAM_MEDIA_H264, 0, 1};
    uint8_t video[40000] = {0}; struct stream_publisher_status status;
    struct stream_address address = stream_address_read(topo_group_wire_id(topo_groups_find(inst->topo_groups, group)));
    sc_public_key_hex(address.ed, viewer_group);
    debug_set_log_hook(viewer_log);
    CHECK(!stream_publisher_open(p, 6, address, &format));
    viewers = 1;
    test_clock_run_for(ua, 11000);
    CHECK(!stream_publisher_snapshot(p, 6, &status) && status.viewers == 1 && viewer_logs == 1);
    viewers = 0;
    test_clock_run_for(ua, 11000);
    CHECK(!stream_publisher_snapshot(p, 6, &status) && status.viewers == 0 && viewer_logs == 2);
    debug_set_log_hook(NULL);
    stream_publisher_stop(p, 6);
    test_clock_run_for(ua, 22000);
    CHECK(!stream_publisher_snapshot(p, 6, &status) && status.state == STREAM_PUBLISHER_ENDED);

    for (unsigned scenario = 0; scenario < 5; scenario++) {
        uint64_t id = scenario + 1;
        rate_cpu = scenario == 0 ? 80 : 81; delay_publication = scenario != 1;
        CHECK(!stream_publisher_open(p, id, address, &format));
        CHECK(stream_publisher_video_rate(p, id, scenario != 3, 1000000, 500000) == 1000000);
        test_clock_run_for(ua, 100);
        for (unsigned tick = 0; tick < 20; tick++) {
            /* Единственная просрочка при высоком CPU не является устойчивой перегрузкой автора. */
            if (scenario != 2 || tick == 0) CHECK(stream_publisher_feed(p, id, 1, get_time_us(), 1, video, sizeof(video), &format) == 1);
            test_clock_run_for(ua, 4000);
            CHECK(!stream_publisher_snapshot(p, id, &status));
            if (tick < 4 || scenario < 3) CHECK(!status.source_overloaded && status.video_bitrate == 1000000);
            if (scenario == 3) CHECK(status.video_bitrate == 1000000);
            if (scenario == 4 && tick >= 5 && tick < 17) CHECK(status.video_bitrate == 800000);
        }
        CHECK(!stream_publisher_snapshot(p, id, &status));
        CHECK(status.source_overloaded == (scenario >= 3));
        if (scenario == 4) {
            CHECK(status.video_bitrate == 640000);
            CHECK(stream_publisher_video_rate(p, id, 1, 1000000, 500000) == 640000);
            CHECK(stream_publisher_video_rate(p, id, 1, 2000000, 500000) == 1280000);
            CHECK(stream_publisher_video_rate(p, id, 1, 1000000, 500000) == 640000);
            for (unsigned tick = 0; tick < 13; tick++) {
                CHECK(stream_publisher_feed(p, id, 1, get_time_us(), 1, video, sizeof(video), &format) == 1);
                test_clock_run_for(ua, 4000);
            }
            CHECK(!stream_publisher_snapshot(p, id, &status) && status.video_bitrate == 512000);
            for (unsigned tick = 0; tick < 13; tick++) {
                CHECK(stream_publisher_feed(p, id, 1, get_time_us(), 1, video, sizeof(video), &format) == 1);
                test_clock_run_for(ua, 4000);
            }
            CHECK(!stream_publisher_snapshot(p, id, &status) && status.video_bitrate == 500000);
            rate_cpu = 70; delay_publication = 0;
            test_clock_run_for(ua, 90000);
            CHECK(!stream_publisher_snapshot(p, id, &status) && status.source_overloaded && status.video_bitrate == 500000);
            test_clock_run_for(ua, 40000);
            CHECK(!stream_publisher_snapshot(p, id, &status) && !status.source_overloaded && status.video_bitrate == 550000);
            test_clock_run_for(ua, 50000);
            CHECK(!stream_publisher_snapshot(p, id, &status) && status.video_bitrate == 600000);
            CHECK(stream_publisher_video_rate(p, id, 0, 1000000, 500000) == 1000000);
        }
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "publisher rate scenario=%u overloaded=%d target=%d", scenario, status.source_overloaded, status.video_bitrate);
        stream_publisher_stop(p, id); test_clock_run_for(ua, 22000);
        CHECK(!stream_publisher_snapshot(p, id, &status) && status.state == STREAM_PUBLISHER_ENDED);
    }
    rate_cpu = -1; delay_publication = 0;
    stream_publisher_destroy(p); uasync_mark_stopped(ua); utun_instance_destroy(inst); uasync_destroy(ua, 0); test_clock_finish();
    CHECK(u_get_allocated_count() == allocated);
}
#endif

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_WARN);
    enum {
        AUDIO_CAPACITY = STREAM_RS_DEFAULT_SIZE - RS_HEADER_SIZE - STREAM_BATCH_OVERHEAD -
            STREAM_MEDIA_HEADER_SIZE - STREAM_MEDIA_AUDIO_SIZE,
        MIXED_CAPACITY = AUDIO_CAPACITY - STREAM_MEDIA_VIDEO_SIZE - 1,
        TWO_FRAMES = AUDIO_CAPACITY - STREAM_MEDIA_AUDIO_SIZE
    };
    const struct scenario cases[] = {
        {"fits-audio-and-video", 1, 0, {MIXED_CAPACITY}, 1, .batches = 1},
        {"fits-audio-only", 1, 0, {MIXED_CAPACITY + 1}, 1, .batches = 1},
        {"exact-capacity", 1, 0, {AUDIO_CAPACITY}, 1, .batches = 1},
        {"unfit-next-fit", 1, 0, {AUDIO_CAPACITY + 1, 320}, 2, .dropped = 1},
        {"unfit-opening", 1, 0, {1275, 320}, 2, .opening = 1, .dropped = 1},
        {"unfit-middle", 1, 0, {320, AUDIO_CAPACITY + 1, 320}, 3, .dropped = 2},
        {"unfit-consecutive", 1, 0, {AUDIO_CAPACITY + 1, 1275, 320}, 3, .dropped = 3},
        {"two-audio-exact", 1, 0, {TWO_FRAMES / 2, TWO_FRAMES - TWO_FRAMES / 2}, 2, .batches = 1},
        {"two-audio-split", 1, 0, {600, 600}, 2, .batches = 2},
        {"mixed-one-byte", 1, 0, {MIXED_CAPACITY}, 1, .video_bytes = 6000},
        {"mixed-audio-only", 1, 0, {MIXED_CAPACITY + 1}, 1, .video_bytes = 6000, .second_video = 1},
        {"mixed-exact", 1, 0, {AUDIO_CAPACITY}, 1, .video_bytes = 6000, .second_video = 1},
        {"large-audio-backlog", 1, 0,
            {AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,
             AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY,AUDIO_CAPACITY},
            32, .video_bytes = 6000, .second_video = 1},
        {"mixed-small-video", 1, 0, {AUDIO_CAPACITY, AUDIO_CAPACITY}, 2, .video_bytes = 5, .second_video = 1},
        {"stop-with-queues", 1, 0, {AUDIO_CAPACITY, 320}, 2, .video_bytes = 6000, .stop = 1, .second_video = 1},
        {"no-fit-stop", 1, 0, {1275}, 1, .stop = 1, .dropped = 1},
        {"opening-large", 1, 0, {AUDIO_CAPACITY}, 1, .video_bytes = 6000, .opening = 1, .second_video = 1},
        {"redundant-one-data", 2, 1, {AUDIO_CAPACITY}, 1, .video_bytes = 6000, .second_video = 1},
        {"two-data-large", 2, 0, {1275}, 1, .video_bytes = 6000},
        {"default-large", 8, 0, {1275, 1275}, 2, .video_bytes = 6000, .batches = 1},
        {"video-only", 1, 0, {0}, 0, .video_bytes = 6000},
        {"pframe-coalescing", 1, 0, {AUDIO_CAPACITY}, 1, .video_bytes = 5, .second_video = 1, .pframe = 1},
        {"audio-eight-limit", 8, 0, {320,320,320,320,320,320,320,320,320,320}, 10, .batches = 2},
        {"required-193", 193, 0, {0}, 0},
        {"required-193-with-parity", 255, 62, {0}, 0}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) check_scenario(&cases[i]);
#ifdef __linux__
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_DEBUG); check_rate();
#endif
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS stream_publisher: %zu cases, real RS/Opus, video progress, OPENING/FIN, no tracked leaks",
        sizeof(cases) / sizeof(cases[0]));
    return 0;
}
