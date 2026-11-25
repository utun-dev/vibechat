/* Общая подготовка: закреплённый target, реальный Opus, отмена и собственный cache. */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/chat_attachment.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/media_async/attachment_send.h"
#include "../src/media_async/media_async.h"
#include "../src/attachments/service.h"
#include "test_utils.h"

#define REQUIRE(value) do { if (!(value)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment send check line=%d", __LINE__); return 1; } } while (0)
struct test_state {
    pthread_t event_thread;
    char target[68];
    int released, release_in_worker, starts, ready, failed;
    int16_t pcm[48000];
};
static struct test_state test;

#ifdef __linux__
static pthread_barrier_t prepared_barrier;
static int capture_work;
static ma_work_fn captured_work;
static unsigned submitted_work;
void __real_media_async_submit(struct media_async*, struct UASYNC*, ma_work_fn, void*, ma_done_fn, void*);
static void prepared_work(void* arg) {
    captured_work(arg);
    pthread_barrier_wait(&prepared_barrier);
}
/* Завершить настоящий crypto worker, сохранив completion до остановки сервиса. */
void __wrap_media_async_submit(struct media_async* ma, struct UASYNC* ua, ma_work_fn work, void* data, ma_done_fn done, void* arg) {
    submitted_work++;
    if (capture_work) { capture_work = 0; captured_work = work; work = prepared_work; }
    __real_media_async_submit(ma, ua, work, data, done, arg);
}
#endif

static void count_temps(void* arg, const char* path, int64_t size) {
    (void)size;
    if (strstr(path, ".tmp.")) (*(unsigned*)arg)++;
}

static void count_files(void* arg, const char* path, int64_t size) {
    (void)path; (void)size; (*(unsigned*)arg)++;
}

static void release_pcm(void* owner) {
    struct test_state* state = owner;
    state->released++; state->release_in_worker = !pthread_equal(pthread_self(), state->event_thread);
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int length) {
    (void)inst;
    if (type != CHAT_EVT_ATTACHMENT_PREPARATION) return;
    if (length < 18 || length != 18 + data[0] || data[0] >= sizeof(test.target)) { test.failed++; return; }
    memcpy(test.target, data + 1, data[0]); test.target[data[0]] = 0;
    int state = data[length - 1];
    if (state == 0) test.starts++;
    else if (state == 1) test.ready++;
    else test.failed++;
}

static int count(struct UTUN_INSTANCE* inst) {
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT count(*) FROM dm_messages", -1, &statement, NULL)) return -1;
    int result = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement); return result;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    test.event_thread = pthread_self();
    char dir[512] = "/tmp/utun_attachment_XXXXXX", config[1024], conv[64];
    REQUIRE(!test_mkdtemp(dir));
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", dir);
    struct UASYNC* ua = uasync_create(); REQUIRE(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); REQUIRE(inst);
    REQUIRE(!utun_core_start(inst) && !chat_service_start(inst));
    chat_event_set_handler(inst, event);
    struct SC_MYKEYS peer; uint8_t ed[32], seed[32];
    REQUIRE(!sc_identity_generate(peer.public_key, peer.private_key, ed, seed));
    uint64_t peer_id = sc_derive_node_id_from_pubkey(peer.public_key);
    REQUIRE(!dm_start(inst, peer_id, peer.public_key, ed, "test", NULL, DM_MODE_DEVICE, NULL));
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(inst->my_ed25519_pubkey, ed));
    REQUIRE(!sc_identity_generate(peer.public_key, peer.private_key, ed, seed));
    peer_id = sc_derive_node_id_from_pubkey(peer.public_key);
    REQUIRE(!dm_start(inst, peer_id, peer.public_key, ed, "other", NULL, DM_MODE_DEVICE, NULL));
    char other[64]; snprintf(other, sizeof(other), "%llu", (unsigned long long)dm_derive_conv_id(inst->my_ed25519_pubkey, ed));
    struct attachment_send_req* req = u_calloc(1, sizeof(*req)); REQUIRE(req);
    req->inst = inst; req->is_dm = 1; req->info.kind = ATTACHMENT_VOICE; req->preset = 1;
    snprintf(req->target, sizeof(req->target), "%s", conv);
    req->pcm = test.pcm; req->pcm_count = 48000; req->pcm_owner = &test; req->pcm_release = release_pcm;
    char active_target[64]; snprintf(active_target, sizeof(active_target), "%s", conv);
    attachment_send_trampoline(req);
    snprintf(active_target, sizeof(active_target), "%s", other); /* Выбор GUI изменился до completion. */
    for (unsigned i = 0; i < 500 && count(inst) == 0; i++) uasync_poll(ua, 20);
    REQUIRE(count(inst) == 1 && test.released == 1 && test.release_in_worker && test.starts == 2 && test.ready == 2 && !test.failed);
    char expected[68], json[8192]; size_t length;
    snprintf(expected, sizeof(expected), "dm:%s", conv);
    REQUIRE(!strcmp(test.target, expected) && strcmp(active_target, conv));
    REQUIRE(!dm_list_messages_json(inst, conv, 20, 0, json, sizeof(json), &length));
    REQUIRE(strstr(json, "\"content_type\":\"audio/opus\"") && strstr(json, "\"duration_ms\":1000"));
    REQUIRE(!dm_list_messages_json(inst, active_target, 20, 0, json, sizeof(json), &length) && !strcmp(json, "[]"));

    /* JPEG/PNG сохраняют метаданные внутри PM ciphertext и удаляют только собственный cache. */
    for (int kind = ATTACHMENT_JPEG; kind <= ATTACHMENT_PNG; ++kind) {
        req = u_calloc(1, sizeof(*req)); REQUIRE(req);
        req->inst = inst; req->is_dm = 1; req->info.kind = kind; req->temporary_source = 1;
        req->info.width = 1080; req->info.height = 1920;
        snprintf(req->target, sizeof(req->target), "%s", conv);
        snprintf(req->source, sizeof(req->source), "%s/image-cache-%d", dir, kind);
        char source_path[1024]; snprintf(source_path, sizeof(source_path), "%s", req->source);
        FILE* image = fopen(source_path, "wb"); REQUIRE(image && fputc(42, image) != EOF && !fclose(image));
        int previous = count(inst);
        attachment_send_trampoline(req);
        for (unsigned i = 0; i < 500 && count(inst) == previous; ++i) uasync_poll(ua, 20);
        REQUIRE(count(inst) == previous + 1 && access(source_path, F_OK) != 0);
        REQUIRE(!dm_list_messages_json(inst, conv, 20, 0, json, sizeof(json), &length));
        REQUIRE(strstr(json, kind == ATTACHMENT_JPEG ? "\"content_type\":\"image/jpeg\"" : "\"content_type\":\"image/png\""));
        REQUIRE(strstr(json, "\"width\":1080,\"height\":1920") && strstr(json, kind == ATTACHMENT_JPEG ? ".jpg" : ".png"));
    }

    /* Готовый MKV копируется без FFmpeg: произвольные байты здесь намеренно не являются видео.
     * Имя/MIME/метаданные должны пережить шифрование PM, временный снимок — удалиться. */
    req = u_calloc(1, sizeof(*req)); REQUIRE(req);
    req->inst = inst; req->is_dm = 1; req->info.kind = ATTACHMENT_VIDEO; req->temporary_source = 1;
    req->info.width = 1920; req->info.height = 1080; req->info.duration_ms = 1234;
    snprintf(req->info.name, sizeof(req->info.name), "recording.mkv");
    snprintf(req->target, sizeof(req->target), "%s", conv);
    snprintf(req->source, sizeof(req->source), "%s/recording-snapshot.mkv", dir);
    char recording_source[1024]; snprintf(recording_source, sizeof(recording_source), "%s", req->source);
    FILE* recording = fopen(recording_source, "wb"); REQUIRE(recording && fputs("original encoded bytes", recording) >= 0 && !fclose(recording));
    attachment_send_trampoline(req);
    for (unsigned i = 0; i < 500 && count(inst) == 3; ++i) uasync_poll(ua, 20);
    REQUIRE(count(inst) == 4 && access(recording_source, F_OK) != 0);
    REQUIRE(!dm_list_messages_json(inst, conv, 20, 0, json, sizeof(json), &length));
    REQUIRE(strstr(json, "\"content_type\":\"video/x-matroska\"") && strstr(json, "recording.mkv"));
    REQUIRE(strstr(json, "\"duration_ms\":1234") && strstr(json, "\"width\":1920,\"height\":1080"));

    /* Остановка до poll завершает preparation с CANCELLED, без регистрации сообщения. */
    chat_core_create_channel_auto(inst, "encrypted cancel");
    sqlite3_stmt* channel_row = NULL;
    REQUIRE(!sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT channel_id FROM channels WHERE name='encrypted cancel'", -1, &channel_row, NULL));
    REQUIRE(sqlite3_step(channel_row) == SQLITE_ROW);
    char channel_id[65]; snprintf(channel_id, sizeof(channel_id), "%s", sqlite3_column_text(channel_row, 0)); sqlite3_finalize(channel_row);
    char prepared[1024]; snprintf(prepared, sizeof(prepared), "%s/cancel-encrypted.bin", dir);
    FILE* prepared_file = fopen(prepared, "wb"); REQUIRE(prepared_file && fputc(42, prepared_file) != EOF && !fclose(prepared_file));
    struct attachment_info encrypted_info = { .kind = ATTACHMENT_FILE, .name = "cancel.bin" }; uint8_t no_reply[32] = {0};
    /* Ошибочный MIME отвергается до передачи source, генерации ключа, файлов и media_async_submit. */
    char too_long[33], unterminated[32]; memset(too_long, 'a', 32); too_long[32] = 0;
    memset(unterminated, 'a', sizeof(unterminated));
    const char* bad_types[] = {NULL, "", "text/plain\n", "text/\"plain", "text/\\plain", "text/\xc3\xa9", too_long, unterminated};
    unsigned files_before = 0; REQUIRE(!ma_dir_walk(dir, count_files, &files_before));
    sqlite3_stmt* keys = NULL;
    REQUIRE(!sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT count(*) FROM attachment_keys", -1, &keys, NULL));
#ifdef __linux__
    unsigned work_before = submitted_work;
#endif
    for (unsigned i = 0; i < sizeof(bad_types) / sizeof(bad_types[0]); i++) {
        REQUIRE(chat_attachment_send(inst, channel_id, prepared, &encrypted_info, bad_types[i], no_reply, 0, NULL) < 0);
        REQUIRE(sqlite3_step(keys) == SQLITE_ROW && sqlite3_column_int(keys, 0) == 0 && !sqlite3_reset(keys));
        unsigned files_after = 0; REQUIRE(!ma_dir_walk(dir, count_files, &files_after) && files_after == files_before);
        prepared_file = fopen(prepared, "rb"); REQUIRE(prepared_file);
        REQUIRE(fgetc(prepared_file) == 42 && fgetc(prepared_file) == EOF && !fclose(prepared_file));
#ifdef __linux__
        REQUIRE(submitted_work == work_before);
#endif
    }
    REQUIRE(!sqlite3_finalize(keys));
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS MIME send:8 invalid types, no keys/workers/files, source preserved");
#ifdef __linux__
    REQUIRE(!pthread_barrier_init(&prepared_barrier, NULL, 2)); capture_work = 1;
#endif
    REQUIRE(!chat_attachment_send(inst, channel_id, prepared, &encrypted_info, "application/octet-stream", no_reply, 0, NULL));
    sqlite3_stmt* paths = NULL;
    REQUIRE(!sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT cipher_path,plain_path FROM attachment_keys", -1, &paths, NULL));
    REQUIRE(sqlite3_step(paths) == SQLITE_ROW);
    char cancelled_cipher[1024], cancelled_plain[1024];
    snprintf(cancelled_cipher, sizeof(cancelled_cipher), "%s", sqlite3_column_text(paths, 0));
    snprintf(cancelled_plain, sizeof(cancelled_plain), "%s", sqlite3_column_text(paths, 1)); sqlite3_finalize(paths);
#ifdef __linux__
    pthread_barrier_wait(&prepared_barrier);
    REQUIRE(ma_file_size(cancelled_cipher) > 0 && ma_file_size(cancelled_plain) == 1);
#endif
    req = u_calloc(1, sizeof(*req)); REQUIRE(req);
    req->inst = inst; req->is_dm = 1; req->info.kind = ATTACHMENT_VOICE; req->preset = 1;
    snprintf(req->target, sizeof(req->target), "%s", conv);
    req->pcm = test.pcm; req->pcm_count = 48000; req->pcm_owner = &test; req->pcm_release = release_pcm;
    attachment_send_trampoline(req);
    chat_service_stop(inst);
    attachment_service_stop(inst);
#ifdef __linux__
    REQUIRE(!pthread_barrier_destroy(&prepared_barrier));
#endif
    REQUIRE(test.released == 2 && test.failed == 0 && count(inst) == 4);
    sqlite3_stmt* cancelled_key = NULL;
    REQUIRE(!sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT count(*) FROM attachment_keys", -1, &cancelled_key, NULL));
    REQUIRE(sqlite3_step(cancelled_key) == SQLITE_ROW && sqlite3_column_int(cancelled_key, 0) == 0); sqlite3_finalize(cancelled_key);
    REQUIRE(ma_file_size(cancelled_cipher) < 0 && ma_file_size(cancelled_plain) < 0 && ma_file_size(prepared) < 0);
    unsigned temps = 0; REQUIRE(!ma_dir_walk(dir, count_temps, &temps) && !temps);
    uasync_poll(ua, 0); REQUIRE(test.failed == 0);

    /* Отклонение после stop удаляет только переданный собственный cache. */
    req = u_calloc(1, sizeof(*req)); REQUIRE(req);
    req->inst = inst; req->is_dm = 1; req->temporary_source = 1; snprintf(req->target, sizeof(req->target), "%s", conv);
    snprintf(req->source, sizeof(req->source), "%s/cache.mp4", dir);
    char cache[1024]; snprintf(cache, sizeof(cache), "%s", req->source);
    FILE* file = fopen(cache, "wb"); REQUIRE(file); REQUIRE(fputc(42, file) != EOF && !fclose(file));
    attachment_send_trampoline(req);
    REQUIRE(test.failed == 0 && access(cache, F_OK) != 0);
    utun_instance_destroy(inst); uasync_destroy(ua, 0);
    printf("test_attachment_send: pinned PM target, worker PCM ownership, cancel and cache cleanup PASS\n");
    return 0;
}
