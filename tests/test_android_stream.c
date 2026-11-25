/* Android actor: локальный preview, очередь, stop/FIN, отменённый open и detach concurrent producer. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/sqlite3.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/chat_setting.h"
#include "../src/stream/stream.h"
#include "../src/stream/stream_member.h"
#include "../src/stream/stream_viewer.h"
#include "../src/utun_instance.h"
#include "../tools/chatgui-android/libutun_lite/stream_bridge.h"
#include "../src/stream/stream_proto.h"
#include "../src/stream/stream_media.h"
#include "test_clock.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android stream test line=%d: %s", __LINE__, #x); abort(); } } while (0)
static uint8_t video[9000];
static struct stream_media_format current_format = {1280, 720, 90, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1};
static int feed(uint64_t id, int is_video, uint64_t pts, int key, const uint8_t *data, size_t length) {
    return android_stream_feed(id, is_video, pts, key, data, length, is_video ? &current_format : NULL);
}
static struct UASYNC *ua;
static int producer_started, producer_stop;
static unsigned source_events;
static uint64_t source_group;
static char *source_snapshot;
struct signal_waiter { struct android_stream_signal *signal; int started, result; };
static void *wait_signal(void *arg) {
    struct signal_waiter *w = arg;
    __atomic_store_n(&w->started, 1, __ATOMIC_RELEASE);
    w->result = android_stream_signal_wait(w->signal, 50); return NULL;
}

static void chat_event(struct UTUN_INSTANCE *inst, int type, const uint8_t *data, int length) {
    if (type != CHAT_EVT_STREAM_SOURCES) return;
    CHECK(length >= 35 && data[32] == 1);
    source_group = stream_group_route(inst, stream_address_read(data)); source_events++;
    u_free(source_snapshot); source_snapshot = u_malloc((uint32_t)(length - 32)); CHECK(source_snapshot);
    memcpy(source_snapshot, data + 33, (size_t)(length - 33)); source_snapshot[length - 33] = '\0';
}

struct catalogue_reply { int done, result; char* json; };
static void catalogue_ready(void* arg, int result, char* json) {
    struct catalogue_reply* reply = arg; CHECK(!reply->done++); reply->result = result; reply->json = json;
}
static void wait_sources(unsigned count) {
    for (unsigned i = 0; i < 1000 && source_events < count; i++) test_clock_poll(ua, 100);
    CHECK(source_events == count);
}

static uint8_t *wait_event(uint64_t id, int kind, size_t *size) {
    for (unsigned i = 0; i < 1000; i++) {
        if (id == 2 || id == 7) CHECK(android_stream_read_request(id, kind == 5 ? 1000000 : -1, 1, 80000, 1) == 0);
        uint8_t *data = android_stream_pull(id, size);
        if (data && data[0] == kind) return data;
        u_free(data); test_clock_poll(ua, 100);
    }
    CHECK(0); return NULL;
}
static void *producer(void *arg) {
    (void)arg;
    __atomic_store_n(&producer_started, 1, __ATOMIC_RELEASE);
    uint64_t pts = 1000000;
    while (!__atomic_load_n(&producer_stop, __ATOMIC_ACQUIRE)) {
        int result = feed(3, 1, pts++, 1, video, sizeof(video));
        if (result < 0) break;
    }
    return NULL;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    size_t before = u_get_allocated_count(); test_clock_init(100); ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE *inst = utun_instance_create_from_str(ua,
        "[global]\nauto_sockets=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n");
    CHECK(inst && !utun_core_start(inst) && !chat_service_start(inst));
    chat_core_create_channel_auto(inst, "Stream \"test\" 😀");
    sqlite3_stmt *statement = NULL;
    CHECK(sqlite3_prepare_v2(chat_core_get_db(inst), "SELECT channel_id FROM channels LIMIT 1", -1, &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    uint64_t group = topo_groups_channel_slot(inst->topo_groups, (const char *)sqlite3_column_text(statement, 0)); sqlite3_finalize(statement); CHECK(group);
    struct stream_address group_key = stream_address_read(topo_group_wire_id(topo_groups_find(inst->topo_groups, group)));
    uasync_mark_running(ua); android_stream_attach(inst);
    chat_event_set_handler(inst, chat_event);
    CHECK(android_stream_watch(group_key) == 0); wait_sources(1);
    CHECK(source_events == 1 && source_group == group && !strcmp(source_snapshot, "[]"));
    CHECK(!stream_service_needs_activity(inst));
    CHECK(android_stream_open(1, group_key, (struct stream_address){{0}}, 1280, 720, 90, 1, 0, STREAM_MEDIA_H264) == 0);
    size_t size = 0; u_free(wait_event(1, 4, &size));
    struct catalogue_reply catalogue = {0}; stream_sources_read(inst, group, catalogue_ready, &catalogue);
    for (unsigned i = 0; i < 1000 && !catalogue.done; i++) test_clock_poll(ua, 100);
    CHECK(catalogue.done && catalogue.result == STREAM_OK && catalogue.json);
    CHECK(strstr(catalogue.json, "epoch") && strstr(catalogue.json, "name")); u_free(catalogue.json); wait_sources(2);
    CHECK(source_events == 2 && strstr(source_snapshot, "epoch"));
    struct stream_member_state member;
    CHECK(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, NULL) == STREAM_OK && member.published);
    CHECK(member.audio_enabled && member.source.n == 8 && member.source.r == 0);
    CHECK(chat_setting_set(inst, "stream_subblocks", "8") == 0);
    CHECK(chat_setting_set(inst, "stream_rs_redundant", "2") == 0);
    CHECK(chat_setting_set(inst, "stream_subblocks", "256") == -2);
    CHECK(chat_setting_set(inst, "stream_rs_redundant", "-1") == -2);
    CHECK(android_stream_set_audio_enabled(1, 0) == 0); test_clock_poll(ua, 100);
    struct stream_member_state muted;
    CHECK(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &muted, NULL) == STREAM_OK && !muted.audio_enabled);
    CHECK(muted.source.source_epoch == member.source.source_epoch && muted.source.n == 8 && muted.source.r == 0);
    CHECK(android_stream_set_audio_enabled(2, 1) < 0);
    CHECK(android_stream_set_audio_enabled(1, 1) == 0); test_clock_poll(ua, 100);
    CHECK(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &muted, NULL) == STREAM_OK && muted.audio_enabled);
    CHECK(android_stream_open(5, group_key, stream_address_read(inst->my_ed25519_pubkey), 0, 0, 0, 1, member.source.source_epoch ^ 1, STREAM_MEDIA_H264) == 0);
    uint8_t *error = wait_event(5, 6, &size); CHECK(error[1] == STREAM_CLOSE_SOURCE_CHANGED); u_free(error);
    CHECK(android_stream_open(2, group_key, stream_address_read(inst->my_ed25519_pubkey), 0, 0, 0, 1, member.source.source_epoch, STREAM_MEDIA_H264) == 0);
    u_free(wait_event(2, 4, &size));
    struct stream_viewer_telemetry telemetry;
    CHECK(android_stream_get_telemetry(2, &telemetry) == 0);
    CHECK(stream_address_equal(telemetry.self, stream_address_read(inst->my_ed25519_pubkey)) && !telemetry.chain.count && telemetry.names[0][0] && telemetry.sample_ms);
    struct android_stream_signal *signal = android_stream_signal_create(2); CHECK(signal);
    CHECK(android_stream_signal_wait(signal, 0) == 0);
    android_stream_signal_notify(signal); android_stream_signal_notify(signal);
    CHECK(android_stream_signal_wait(signal, 0) == 1 && android_stream_signal_wait(signal, 0) == 0);
    CHECK(android_stream_open(7, group_key, stream_address_read(inst->my_ed25519_pubkey), 0, 0, 0, 1, member.source.source_epoch, STREAM_MEDIA_H264) == 0);
    u_free(wait_event(7, 4, &size));
    CHECK(android_stream_open(8, group_key, stream_address_read(inst->my_ed25519_pubkey), 0, 0, 0, 1, member.source.source_epoch, STREAM_MEDIA_H264) == 0);
    android_stream_stop(8);
    CHECK(source_events == 2); /* Подписка не меняет каталог публикаций. */
    for (unsigned i = 0; i < sizeof(video); i++) video[i] = (uint8_t)i;
    CHECK(feed(1, 1, 100000, 0, video, sizeof(video)) == 0);
    CHECK(feed(1, 1, 100000, 1, video, sizeof(video)) == 1);
    CHECK(feed(1, 0, 100000, 0, (const uint8_t *)"ops", 3) == 1);
    uint8_t *output = NULL; int video_seen = 0, audio_seen = 0;
    for (unsigned i = 0; i < 1000 && (!video_seen || !audio_seen); i++) {
        CHECK(android_stream_read_request(2, 100000, 1, 80000, 1) == 0);
        output = android_stream_pull(2, &size);
        if (output && output[0] == 1) {
            CHECK(size == sizeof(video) + ANDROID_STREAM_HEADER_SIZE && stream_read_be(output + 8, 8) == 100000 && !memcmp(output + ANDROID_STREAM_HEADER_SIZE, video, sizeof(video)));
            video_seen++;
        } else if (output && output[0] == 2) { CHECK(size == 3 + ANDROID_STREAM_HEADER_SIZE && !memcmp(output + ANDROID_STREAM_HEADER_SIZE, "ops", 3)); audio_seen++; }
        u_free(output); test_clock_poll(ua, 100);
    }
    CHECK(video_seen == 1 && audio_seen == 1);
    output = wait_event(7, 1, &size);
    CHECK(stream_read_be(output + 8, 8) == 100000 && !memcmp(output + ANDROID_STREAM_HEADER_SIZE, video, sizeof(video)));
    u_free(output); android_stream_stop(7); test_clock_poll(ua, 100);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "Android independent viewers: delayed reader consumed its own frame; primary remains active");
    /* Commit новой revision удаляет старое видео, сохраняет аудио/epoch и не требует нового owner. */
    CHECK(feed(1, 1, 200000, 0, video, sizeof(video)) == 1);
    CHECK(feed(1, 0, 120000, 0, (const uint8_t *)"ops", 3) == 1);
    struct stream_media_format next_format = {1920, 1080, 270, STREAM_MEDIA_H265, STREAM_MEDIA_OPUS_STEREO, 2};
    CHECK(android_stream_feed(1, 1, 300000, 0, video, sizeof(video), &next_format) == 0);
    CHECK(android_stream_feed(1, 1, 300000, 1, video, sizeof(video), &next_format) == 1);
    CHECK(feed(1, 1, 350000, 1, video, sizeof(video)) == 0);
    next_format.width++;
    CHECK(android_stream_feed(1, 1, 400000, 1, video, sizeof(video), &next_format) == -1);
    next_format.width--;
    next_format.video_codec = STREAM_MEDIA_VP9;
    CHECK(android_stream_feed(1, 1, 400000, 1, video, sizeof(video), &next_format) == -1);
    next_format.video_codec = STREAM_MEDIA_H265;
    video_seen = audio_seen = 0;
    for (unsigned i = 0; i < 1000 && (!video_seen || !audio_seen); i++) {
        CHECK(android_stream_read_request(2, 300000, 1, 80000, 1) == 0);
        output = android_stream_pull(2, &size);
        if (output && output[0] == 1) {
            CHECK(stream_read_be(output + 8, 8) == 300000 && stream_read_be(output + 20, 4) == 2 && output[24] == STREAM_MEDIA_H265);
            CHECK(stream_read_be(output + 2, 2) == 1920 && stream_read_be(output + 6, 2) == 270); video_seen++;
        } else if (output && output[0] == 2) { CHECK(stream_read_be(output + 8, 8) == 120000); audio_seen++; }
        else if (output) CHECK(output[0] != 6);
        u_free(output); test_clock_poll(ua, 100);
    }
    CHECK(video_seen == 1 && audio_seen == 1);
    /* Ручной поворот при том же разрешении: новый KEY, непрерывное аудио и тот же viewer. */
    const uint16_t rotations[] = {0, 90, 180, 270};
    for (unsigned turn = 0; turn < 4; turn++) {
        next_format.revision++; next_format.rotation = rotations[turn];
        uint64_t video_pts = 400000 + turn * 100000, audio_pts = 140000 + turn * 20000;
        CHECK(android_stream_feed(1, 1, video_pts, 0, video, sizeof(video), &next_format) == 0);
        CHECK(android_stream_feed(1, 1, video_pts, 1, video, sizeof(video), &next_format) == 1);
        CHECK(feed(1, 0, audio_pts, 0, (const uint8_t *)"ops", 3) == 1);
        video_seen = audio_seen = 0;
        for (unsigned i = 0; i < 1000 && (!video_seen || !audio_seen); i++) {
            CHECK(android_stream_read_request(2, (int64_t)video_pts, 1, 80000, 1) == 0);
            output = android_stream_pull(2, &size);
            if (output && output[0] == 1) {
                CHECK(stream_read_be(output + 8, 8) == video_pts && stream_read_be(output + 20, 4) == next_format.revision);
                CHECK(stream_read_be(output + 2, 2) == 1920 && stream_read_be(output + 4, 2) == 1080);
                CHECK(stream_read_be(output + 6, 2) == rotations[turn] && (output[1] & STREAM_MEDIA_KEY)); video_seen++;
            } else if (output && output[0] == 2) { CHECK(stream_read_be(output + 8, 8) == audio_pts); audio_seen++; }
            else if (output) CHECK(output[0] != 6);
            u_free(output); test_clock_poll(ua, 100);
        }
        CHECK(video_seen == 1 && audio_seen == 1);
    }
    struct stream_member_state after_switch;
    CHECK(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &after_switch, NULL) == STREAM_OK);
    CHECK(after_switch.published && after_switch.source.source_epoch == member.source.source_epoch);
    /* Повторные запросы готовят один ответ; получение ответа не создаёт скрытую следующую копию. */
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_DEBUG);
    for (unsigned i = 0; i < 5; i++)
        CHECK(android_stream_feed(1, 1, 800000 + i * 40000, 0, video, sizeof(video), &next_format) == 1);
    for (unsigned i = 0; i < 50; i++) { CHECK(android_stream_read_request(2, 700000, 1, -1, 1) == 0); test_clock_poll(ua, 100); }
    output = android_stream_pull(2, &size); CHECK(output && output[0] == 1 && stream_read_be(output + 8, 8) == 800000); u_free(output);
    /* AVAILABLE между pull и отменой demand раньше выдавал второй кадр при занятом decoder input. */
    CHECK(android_stream_feed(1, 1, 1000000, 0, video, sizeof(video), &next_format) == 1);
    test_clock_run_for(ua, 1000);
    output = android_stream_pull(2, &size);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "demand regression: new data after reply, unsolicited_kind=%d", output ? output[0] : 0);
    CHECK(!output);
    test_clock_run_for(ua, 1000); CHECK(!android_stream_pull(2, &size));
    CHECK(android_stream_read_request(2, 700000, 0, -1, 1) == 0); test_clock_run_for(ua, 1000);
    CHECK(!android_stream_pull(2, &size));
    for (unsigned frame = 1; frame < 6; frame++) {
        output = NULL;
        for (unsigned i = 0; i < 1000 && !output; i++) {
            CHECK(android_stream_read_request(2, 700000, 1, -1, 1) == 0); test_clock_poll(ua, 100); output = android_stream_pull(2, &size);
        }
        CHECK(output && output[0] == 1 && stream_read_be(output + 8, 8) == 800000 + frame * 40000); u_free(output);
    }
    /* Пустой pull не отменяет WAIT: уже запрошенный кадр приходит после появления данных. */
    CHECK(android_stream_read_request(2, 1000000, 1, -1, 1) == 0); test_clock_run_for(ua, 1000);
    CHECK(!android_stream_pull(2, &size));
    CHECK(android_stream_feed(1, 1, 1040000, 0, video, sizeof(video), &next_format) == 1);
    test_clock_run_for(ua, 1000);
    output = android_stream_pull(2, &size);
    CHECK(output && output[0] == 1 && stream_read_be(output + 8, 8) == 1040000); u_free(output);
    /* Отмена WAIT сохраняет следующий кадр, а запрос только аудио не забирает видео. */
    CHECK(android_stream_read_request(2, 1040000, 1, -1, 1) == 0); test_clock_run_for(ua, 1000);
    CHECK(!android_stream_pull(2, &size)); CHECK(android_stream_read_request(2, 1040000, 0, -1, 1) == 0);
    CHECK(android_stream_feed(1, 1, 1080000, 0, video, sizeof(video), &next_format) == 1);
    CHECK(feed(1, 0, 220000, 0, (const uint8_t *)"ops", 3) == 1); test_clock_run_for(ua, 1000);
    CHECK(!android_stream_pull(2, &size));
    CHECK(android_stream_read_request(2, 1040000, 0, 80000, 1) == 0); test_clock_run_for(ua, 1000);
    output = android_stream_pull(2, &size);
    CHECK(output && output[0] == 2 && stream_read_be(output + 8, 8) == 220000); u_free(output);
    CHECK(android_stream_read_request(2, 1040000, 1, -1, 1) == 0); test_clock_run_for(ua, 1000);
    output = android_stream_pull(2, &size);
    CHECK(output && output[0] == 1 && stream_read_be(output + 8, 8) == 1080000); u_free(output);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS demand regression: one reply, WAIT survives empty pull, cancellation and audio-only preserve video");
    for (unsigned i = 0; i < 3; i++) {
        CHECK(android_stream_feed(1, 1, 1120000 + i * 40000, 0, video, sizeof(video), &next_format) == 1);
        CHECK(feed(1, 0, 240000 + i * 20000, 0, (const uint8_t *)"ops", 3) == 1);
    }
    test_clock_run_for(ua, 1000);
    CHECK(android_stream_signal_wait(signal, 0) >= 0);
    unsigned batch_video = 0, batch_audio = 0, largest = 0;
    for (unsigned attempt = 0; attempt < 20 && (batch_video < 3 || batch_audio < 3); attempt++) {
        CHECK(android_stream_read_request(2, 1080000, 3 - batch_video, 80000, 16) == 0);
        test_clock_run_for(ua, 1000);
        output = android_stream_pull(2, &size); CHECK(output);
        CHECK(android_stream_signal_wait(signal, 0) == 1);
        unsigned records = 0;
        for (size_t offset = 0; offset < size;) {
            CHECK(size - offset >= ANDROID_STREAM_HEADER_SIZE);
            size_t length = (size_t)stream_read_be(output + offset + 16, 4);
            CHECK(length <= size - offset - ANDROID_STREAM_HEADER_SIZE);
            uint64_t pts = stream_read_be(output + offset + 8, 8);
            if (output[offset] == 1) {
                CHECK(pts == 1120000 + batch_video * 40000 && length == sizeof(video)); batch_video++;
            } else { CHECK(output[offset] == 2 && pts == 240000 + batch_audio * 20000 && length == 3); batch_audio++; }
            records++; offset += ANDROID_STREAM_HEADER_SIZE + length;
        }
        CHECK(records <= 16); if (records > largest) largest = records; u_free(output);
    }
    CHECK(batch_video == 3 && batch_audio == 3 && largest > 1);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS Android batches: video=%u audio=%u largest=%u notified=1", batch_video, batch_audio, largest);
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    android_stream_stop(1); u_free(wait_event(2, 5, &size));
    CHECK(android_stream_signal_wait(signal, 0) >= 0);
    struct signal_waiter waiter = {.signal = signal}; pthread_t waiter_thread;
    CHECK(pthread_create(&waiter_thread, NULL, wait_signal, &waiter) == 0);
    while (!__atomic_load_n(&waiter.started, __ATOMIC_ACQUIRE)) {}
    android_stream_stop(2); CHECK(pthread_join(waiter_thread, NULL) == 0 && waiter.result == 1);
    android_stream_signal_destroy(signal); u_free(wait_event(1, 5, &size));
    while ((output = android_stream_pull(2, &size))) u_free(output);
    CHECK(!strcmp(source_snapshot, "[]"));
    current_format.rotation = 0; current_format.audio_codec = 0;
    CHECK(android_stream_open(3, group_key, (struct stream_address){{0}}, 1280, 720, 0, 0, 0, STREAM_MEDIA_H264) == 0); u_free(wait_event(3, 4, &size));
    CHECK(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, NULL) == STREAM_OK);
    CHECK(member.source.n == 8 && member.source.r == 2);
    CHECK(android_stream_set_audio_enabled(3, 1) < 0);
    uint8_t *large = u_malloc(500000); CHECK(large); memset(large, 1, 500000);
    CHECK(feed(3, 1, 500000, 1, large, 500000) == 1); u_free(large);
    /* Getter копирует периодический снимок: ждём первый sample, а не границу его таймера в 100мс. */
    struct android_stream_tx_stats stats = {0}; int stats_rc; uint64_t stats_deadline = get_time_tb() + 2000;
    do { test_clock_poll(ua, 10); stats_rc = android_stream_tx_snapshot(3, &stats); }
    while (!stats_rc && !stats.sample_us && get_time_tb() < stats_deadline);
    if (stats_rc || !stats.sample_us || !stats.video_bytes || stats.video_bytes >= 500000) DEBUG_ERROR(DEBUG_CATEGORY_STREAM,
        "Android stream TX snapshot: rc=%d video=%llu total=%llu acked=%llu connections=%u sample=%llu now=%llu", stats_rc,
        (unsigned long long)stats.video_bytes, (unsigned long long)stats.total_bytes,
        (unsigned long long)stats.acked_1s, stats.connections, (unsigned long long)stats.sample_us,
        (unsigned long long)get_time_us());
    CHECK(stats_rc == 0 && stats.sample_us && stats.video_bytes > 0 && stats.video_bytes < 500000);
    CHECK(stats.total_bytes == stats.video_bytes && !stats.acked_1s && !stats.connections);
    pthread_t thread; CHECK(pthread_create(&thread, NULL, producer, NULL) == 0);
    while (!__atomic_load_n(&producer_started, __ATOMIC_ACQUIRE)) {}
    android_stream_detach(inst);
    __atomic_store_n(&producer_stop, 1, __ATOMIC_RELEASE); CHECK(pthread_join(thread, NULL) == 0);
    CHECK(feed(3, 1, 900000, 1, video, sizeof(video)) == -1);
    CHECK(android_stream_set_audio_enabled(3, 0) < 0);
    while ((output = android_stream_pull(3, &size))) u_free(output);
    android_stream_attach(inst);
    CHECK(android_stream_open(4, group_key, (struct stream_address){{0}}, 1280, 720, 0, 0, 0, STREAM_MEDIA_H264) == 0); android_stream_stop(4);
    u_free(wait_event(4, 5, &size));
    /* Маленькая пачка и пачка сверх предела медиаблока: полный кадр проходит через локальный viewer. */
    const unsigned totals[] = {1, 16, 64};
    for (unsigned test = 0; test < sizeof(totals) / sizeof(totals[0]); test++) {
        unsigned total = totals[test], parity = total == 16 ? 4 : 0;
        char setting[8]; snprintf(setting, sizeof(setting), "%u", total);
        CHECK(chat_setting_set(inst, "stream_subblocks", setting) == 0);
        snprintf(setting, sizeof(setting), "%u", parity);
        CHECK(chat_setting_set(inst, "stream_rs_redundant", setting) == 0);
        CHECK(android_stream_open(6, group_key, (struct stream_address){{0}}, 1280, 720, 0, 0, 0, STREAM_MEDIA_H264) == 0);
        u_free(wait_event(6, 4, &size));
        CHECK(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, NULL) == STREAM_OK);
        CHECK(member.source.n == total && member.source.r == parity);
        struct STREAM_HANDLE *reader = NULL;
        CHECK(stream_media_open(inst, group, stream_address_read(inst->my_ed25519_pubkey), member.source.source_epoch, STREAM_MEDIA_TRACK_VIDEO, &reader) == STREAM_OK);
        size_t length = total == 1 ? sizeof(video) : 100000;
        uint8_t *frame = u_malloc(length); CHECK(frame); memset(frame, 7, length);
        CHECK(feed(6, 1, 100000, 1, frame, length) == 1);
        struct stream_media_packet packet; int received = 0;
        for (unsigned i = 0; i < 1000 && !received; i++) {
            test_clock_poll(ua, 100);
            int result = stream_media_read(reader, 100000, 1, -1, &packet);
            if (result == STREAM_OK) {
                CHECK(packet.kind == STREAM_MEDIA_READ_VIDEO && packet.video.length == length && !memcmp(packet.video.data, frame, length)); received = 1;
            } else CHECK(result == STREAM_WAIT || result == STREAM_MORE);
        }
        CHECK(received); u_free(frame); stream_close(reader);
        android_stream_stop(6); u_free(wait_event(6, 5, &size));
    }
    android_stream_detach(inst); uasync_mark_stopped(ua); utun_instance_destroy(inst); uasync_destroy(ua, 0);
    u_free(source_snapshot);
    CHECK(u_get_allocated_count() == before);
    test_clock_finish();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "PASS Android stream: preview, source snapshot, FIN, cancellation, concurrent detach and no leaks");
    return 0;
}
