/* Настоящий service callback: FILE/RW, нет SQLite в uasync, send/edit и durable ошибки. */
#include "../src/utun_instance.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/chat_sync.h"
#include "../src/chat/chat_poll.h"
#include "../src/chat/chat_channel.h"
#include "../src/media_delivery/media_download.h"
#include "../src/media_delivery/media_index.h"
#include "../src/attachments/download.h"
#include "../lib/file_io.h"
#include "../src/group/group_protocol.h"
#include "../src/db_storage.h"
#include "../lib/db_sql.h"
#include "../lib/mem.h"
#include "../lib/json_flat.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <time.h>

static pthread_t owner;
static int forbid_owner_sql, fault, fail_alloc;
static int hold_preflight, preflight_entered, preflight_resets;
static int hold_edit, edit_entered;
static int hold_scan, scan_entered;
static int base_finalize_fault;
static pthread_mutex_t preflight_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t preflight_condition = PTHREAD_COND_INITIALIZER;
static void* retry_timer;
static void* retry_arg;
static void (*retry_callback)(void*);
static unsigned received, updated;
static unsigned ready_events;
static unsigned download_events;
static int capture_download;
static void (*manual_done)(void*, int);
static void* manual_arg;
static char manual_path[MEDIA_PATH_MAX];
int __real_media_download_start(struct UTUN_INSTANCE*, uint64_t, const struct media_index_result*, const char*, const char*,
    const uint8_t*, void (*)(void*,int), void*, void (*)(void*,int,int), void*);
int __wrap_media_download_start(struct UTUN_INSTANCE* inst, uint64_t group, const struct media_index_result* result,
    const char* path, const char* base, const uint8_t author[32], void (*done)(void*,int), void* arg,
    void (*progress)(void*,int,int), void* progress_arg) {
    if (!capture_download) return __real_media_download_start(inst, group, result, path, base, author, done, arg, progress, progress_arg);
    assert(!manual_done && result->num_blocks == 1 && result->file_size == 17 && group && author && progress && progress_arg == arg);
    capture_download = 0; manual_done = done; manual_arg = arg; snprintf(manual_path, sizeof(manual_path), "%s", path); return 0;
}
static int take(int value) { int expected = value; return __atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0); }
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (forbid_owner_sql && pthread_equal(owner, pthread_self()))
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "unexpected owner SQL db=%p: %.220s", (void*)db, sql);
    assert(!forbid_owner_sql || !pthread_equal(owner, pthread_self()));
    if (strstr(sql, "INSERT INTO \"msg_") && take(1)) return SQLITE_BUSY;
    if (strstr(sql, "INSERT INTO \"msg_") && take(3)) return SQLITE_IOERR;
    if (strstr(sql, "INSERT OR REPLACE INTO local_identity") && take(6)) return SQLITE_BUSY;
    if (strstr(sql, "SELECT adm_tags,adm_tags_sig") && take(7)) return SQLITE_BUSY;
    if (strstr(sql, "SELECT adm_tags,adm_tags_sig") && take(8)) return SQLITE_IOERR;
    if (strstr(sql, "SELECT x25519_pubkey,ed25519_pubkey,node_id FROM peers_") && take(4)) return SQLITE_IOERR;
    if (strstr(sql, "SELECT x25519_pubkey,ed25519_pubkey,node_id FROM peers_") && take(5)) return SQLITE_CORRUPT;
    if (strstr(sql, "SELECT archived FROM history_msg_") && take(9)) return SQLITE_BUSY;
    if (strstr(sql, "SELECT message_id,author_ed,kind,ver,data,signature FROM feedback_") && take(10)) return SQLITE_BUSY;
    if (strstr(sql, "SELECT json_valid(?1)") && take(11)) return SQLITE_IOERR;
    if (strstr(sql, "SELECT json_valid(?1)") && take(12)) return SQLITE_BUSY;
    if (strstr(sql, "SELECT author_ed,kind,ver,data FROM feedback_") && take(13)) return SQLITE_BUSY;
    if (strstr(sql, "SELECT channel_id FROM channels WHERE EXISTS") && take(14)) return SQLITE_BUSY;
    if (strstr(sql, "SELECT channel_id FROM channels WHERE EXISTS") && take(15)) return SQLITE_IOERR;
    if (strstr(sql, "SELECT channel_id FROM channels WHERE EXISTS") && __atomic_load_n(&hold_scan, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&preflight_mutex); __atomic_store_n(&scan_entered, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&hold_scan, __ATOMIC_SEQ_CST)) pthread_cond_wait(&preflight_condition, &preflight_mutex);
        pthread_mutex_unlock(&preflight_mutex);
    }
    if (strstr(sql, "SELECT timestamp,accepted_key,nonce,ver,edited_at,") && __atomic_load_n(&hold_edit, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&preflight_mutex); __atomic_store_n(&edit_entered, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&hold_edit, __ATOMIC_SEQ_CST)) pthread_cond_wait(&preflight_condition, &preflight_mutex);
        pthread_mutex_unlock(&preflight_mutex);
    }
    if (strstr(sql, "DELETE FROM merkle_tree_hash WHERE namespace=") &&
        (__atomic_load_n(&hold_preflight, __ATOMIC_SEQ_CST) || __atomic_load_n(&preflight_entered, __ATOMIC_SEQ_CST)))
        __atomic_add_fetch(&preflight_resets, 1, __ATOMIC_SEQ_CST);
    if (strstr(sql, "FROM \"msg_") && strstr(sql, "ORDER BY rowid LIMIT 16") && __atomic_load_n(&hold_preflight, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&preflight_mutex); __atomic_store_n(&preflight_entered, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&hold_preflight, __ATOMIC_SEQ_CST)) pthread_cond_wait(&preflight_condition, &preflight_mutex);
        pthread_mutex_unlock(&preflight_mutex);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
void* __real_uasync_set_timeout(struct UASYNC*, uint64_t, void*, void (*)(void*), const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, uint64_t delay, void* arg, void (*callback)(void*), const char* name) {
    void* timer = __real_uasync_set_timeout(ua, delay, arg, callback, name);
    if (!strcmp(name, "history_retry")) { retry_timer = timer; retry_arg = arg; retry_callback = callback; }
    return timer;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*,int,char**,char**), void* arg, char** error) {
    assert(!forbid_owner_sql || !pthread_equal(owner, pthread_self()));
    return !strcmp(sql, "COMMIT") && take(2) ? SQLITE_BUSY : __real_sqlite3_exec(db, sql, cb, arg, error);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int selected = st && !strcmp(sqlite3_sql(st), "SELECT value FROM ui_state WHERE key='media_base'");
    int rc = __real_sqlite3_finalize(st);
    if (selected && __atomic_load_n(&base_finalize_fault, __ATOMIC_SEQ_CST) > 0 &&
        __atomic_sub_fetch(&base_finalize_fault, 1, __ATOMIC_SEQ_CST) == 0) return SQLITE_IOERR;
    return rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t bytes, const char* location) {
    if (fail_alloc == 3 && pthread_equal(owner, pthread_self()) && strstr(location, "chat_msg.c")) { fail_alloc = 0; return NULL; }
    if (fail_alloc == 4 && pthread_equal(owner, pthread_self()) && strstr(location, "chat_poll.c")) { fail_alloc = 0; return NULL; }
    if (fail_alloc == 2 && pthread_equal(owner, pthread_self()) && strstr(location, "chat_core.c")) { fail_alloc = 0; return NULL; }
    if (fail_alloc && pthread_equal(owner, pthread_self()) && strstr(location, "chat_message.c")) { fail_alloc = 0; return NULL; }
    return __real_u_calloc_impl(n, bytes, location);
}
struct reply { int calls, expected, expected_code; struct chat_message_result result; };
struct member_reply { int calls, status, code; };
static void member_saved(void* data, const struct db_async_result* result) {
    struct member_reply* reply = data;
    if (reply->status != result->status || reply->code != result->sqlite_code)
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "member result expected=%d/%d actual=%d/%d: %s", reply->status, reply->code,
            result->status, result->sqlite_code, result->message);
    assert(pthread_equal(owner, pthread_self()) && !reply->calls++ && result->status == reply->status && result->sqlite_code == reply->code);
}
struct member_probe { char channel[65]; uint64_t node; int version; };
struct attrs_probe { char channel[65], json[1024]; uint8_t message[32], signature[64]; uint64_t timestamp; int64_t id; };
static int attrs_probe_work(sqlite3* db, void* data) {
    struct attrs_probe* probe = data; sqlite3_stmt* st = NULL; char sql[192];
    snprintf(sql, sizeof(sql), "SELECT timestamp,author_signature,local_attrs,id FROM history_msg_%s WHERE message_id=?", probe->channel);
    assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && !sqlite3_bind_blob(st, 1, probe->message, 32, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 1) == 64);
    probe->timestamp = (uint64_t)sqlite3_column_int64(st, 0);
    probe->id = sqlite3_column_int64(st, 3);
    memcpy(probe->signature, sqlite3_column_blob(st, 1), 64);
    const char* json = (const char*)sqlite3_column_text(st, 2); snprintf(probe->json, sizeof(probe->json), "%s", json ? json : "");
    return sqlite3_finalize(st);
}
static void probe_done(void* data, const struct db_async_result* result) { (void)data; assert(!result->status); }
static int member_probe_work(sqlite3* db, void* data) {
    struct member_probe* probe = data; char sql[256]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT local_nick,json_extract(adm_tags,'$.worker_note'),json_extract(adm_tags,'$.ver')"
        " FROM peers_%s WHERE node_id=?", probe->channel);
    assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && !sqlite3_bind_int64(st, 1, (sqlite3_int64)probe->node));
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "copied nickname") &&
        !strcmp((const char*)sqlite3_column_text(st, 1), "copied note"));
    probe->version = sqlite3_column_int(st, 2); assert(probe->version > 0); return sqlite3_finalize(st);
}
static void stop_groups(void* data, const struct db_async_result* result) {
    assert(result->status == DB_ASYNC_CANCELLED); ((struct UTUN_INSTANCE*)data)->group_started = 0;
}
static void saved(void* data, const struct chat_message_result* result) {
    struct reply* reply = data;
    if (result->status != reply->expected || (reply->expected_code && result->sqlite_code != reply->expected_code))
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "message test result expected=%d/%d actual=%d/%d committed=%d", reply->expected,
            reply->expected_code, result->status, result->sqlite_code, result->committed);
    assert(pthread_equal(owner, pthread_self()) && !reply->calls++ && result->status == reply->expected);
    if (reply->expected_code) assert(result->sqlite_code == reply->expected_code);
    reply->result = *result;
}
static void ready(void* data, const struct db_async_result* result) { assert(!result->status); *(int*)data = 1; }
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    (void)inst; (void)data; (void)bytes;
    if (type == CHAT_EVT_MSG_RECEIVED) received++;
    if (type == CHAT_EVT_MSG_UPDATED) updated++;
    if (type == CHAT_EVT_ATTACHMENT_DOWNLOADED) download_events++;
    if (type == CHAT_EVT_DB_READY) ready_events++;
}
static void drain(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
struct feedback_reply { int calls, expected, code, committed; char json[16384]; };
static void feedback_done(void* data, const struct chat_feedback_command_result* result) {
    struct feedback_reply* reply = data;
    if (result->status != reply->expected || result->sqlite_code != reply->code) DEBUG_ERROR(DEBUG_CATEGORY_CHAT,
        "feedback expected=%d/%d got=%d/%d committed=%d JSON=%s", reply->expected, reply->code, result->status,
        result->sqlite_code, result->committed, result->json ? result->json : "null");
    assert(!reply->calls++ && result->status == reply->expected && result->sqlite_code == reply->code);
    reply->committed = result->committed; snprintf(reply->json, sizeof(reply->json), "%s", result->json ? result->json : "");
}
static void feedback_group_created(void* data, const struct chat_channel_result* result) {
    assert(!result->status && result->committed && result->channel[0]); strcpy(data, result->channel);
}
static void feedback_cases(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], const char* path) {
    char command[512], hex[65]; for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", message[i]);
    struct feedback_reply reply = {0};
    chat_feedback_command_submit(inst, "{\"cmd\":\"reaction_catalog\"}", feedback_done, &reply); drain(inst);
    assert(reply.calls == 1 && !reply.committed && strstr(reply.json, "catalog"));
    snprintf(command, sizeof(command), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":0}", channel, hex);
    reply = (struct feedback_reply){0}; chat_feedback_command_submit(inst, command, feedback_done, &reply);
    memset(command, 'x', strlen(command)); drain(inst); assert(reply.calls == 1 && reply.committed && strstr(reply.json, "\"reaction_ver\":3"));
    snprintf(command, sizeof(command), "{\"cmd\":\"react\",\"ch\":\"%s\",\"message_id\":\"%s\",\"emoji\":1}", channel, hex);
    for (int point = 0; point < 2; point++) {
        fault = point ? 2 : 10; reply = (struct feedback_reply){.expected=-1,.code=SQLITE_BUSY};
        chat_feedback_command_submit(inst, command, feedback_done, &reply); drain(inst); assert(reply.calls == 1 && !reply.committed && !fault);
    }
    reply = (struct feedback_reply){0}; chat_feedback_command_submit(inst, command, feedback_done, &reply); drain(inst);
    assert(reply.calls == 1 && reply.committed && strstr(reply.json, "\"reaction_ver\":4"));
    fail_alloc = 4; reply = (struct feedback_reply){.expected=-1,.code=SQLITE_NOMEM};
    chat_feedback_command_submit(inst, command, feedback_done, &reply); assert(reply.calls == 1 && !fail_alloc && !reply.committed);
    fault = 12; reply = (struct feedback_reply){.expected=-1,.code=SQLITE_BUSY};
    chat_feedback_command_submit(inst, command, feedback_done, &reply); drain(inst); assert(reply.calls == 1 && !fault);
    fault = 13; reply = (struct feedback_reply){.code=SQLITE_BUSY};
    chat_feedback_command_submit(inst, command, feedback_done, &reply); drain(inst);
    assert(reply.calls == 1 && reply.committed && !fault && strstr(reply.json, "\"snapshot_pending\":true"));
    char group[65] = ""; chat_channel_create_auto_submit(inst, "FILE poll", "chat", feedback_group_created, group); drain(inst); assert(group[0]);
    char poll_channel[65]; strcpy(poll_channel, group);
    snprintf(command, sizeof(command), "{\"cmd\":\"poll_create\",\"ch\":\"%s\",\"question\":\"Worker poll\",\"mode\":\"single\",\"options\":[\"one\",\"two\"]}", poll_channel);
    reply = (struct feedback_reply){0}; chat_feedback_command_submit(inst, command, feedback_done, &reply); drain(inst);
    assert(reply.calls == 1 && reply.committed && strstr(reply.json, "Worker poll"));
    char poll_id[65]; assert(!json_flat_get(reply.json, "message_id", poll_id, sizeof(poll_id)));
    snprintf(command, sizeof(command), "{\"cmd\":\"vote\",\"ch\":\"%s\",\"message_id\":\"%s\",\"choices\":[0]}", poll_channel, poll_id);
    reply = (struct feedback_reply){0}; chat_feedback_command_submit(inst, command, feedback_done, &reply); drain(inst);
    if (!strstr(reply.json, "\"ver\":3,\"mine\"")) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "poll vote snapshot: %s", reply.json);
    assert(reply.calls == 1 && reply.committed && strstr(reply.json, "\"ver\":3,\"mine\""));
    fault = 11; reply = (struct feedback_reply){.expected=-1,.code=SQLITE_IOERR};
    chat_feedback_command_submit(inst, command, feedback_done, &reply); drain(inst); assert(reply.calls == 1 && !fault && !reply.committed);
    reply = (struct feedback_reply){.expected=-1,.code=SQLITE_IOERR}; chat_feedback_command_submit(inst, command, feedback_done, &reply);
    assert(reply.calls == 1 && !reply.committed);
    assert(!db_async_destroy(inst->db_worker)); int opened = 0; inst->db_worker = db_async_create(inst->ua, path, 16, ready, &opened);
    assert(inst->db_worker); drain(inst); assert(opened);
    puts("PASS FILE feedback commands: copied JSON, signed atomic toggle/version, SQL/COMMIT rollback, OOM and owner SQL forbidden");
}
static int cancel(void* data) { (void)data; return 0; }
static int never(sqlite3* db, void* data) { (void)db; (void)data; assert(0); return SQLITE_ERROR; }
static void generation(void* data, const struct db_async_result* result) {
    struct UTUN_INSTANCE* inst = data; assert(result->status == DB_ASYNC_CANCELLED); inst->group_generation++;
}
static void stop_content(void* data, const struct db_async_result* result) {
    assert(result->status == DB_ASYNC_CANCELLED); ((struct UTUN_INSTANCE*)data)->chat_content_started = 0;
}
static void ignore(void* data, const struct db_async_result* result) { (void)data; assert(result->status == DB_ASYNC_CANCELLED); }
struct replacement { struct UTUN_INSTANCE* inst; struct db_sync_bootstrap prepared; char channel[65]; };
static int prepare_history(sqlite3* db, void* data) {
    struct replacement* replacement = data;
    return db_sync_instance_prepare(db, replacement->prepared.table, replacement->prepared.channel, &replacement->prepared);
}
static void replace_history(void* data, const struct db_async_result* result) {
    struct replacement* replacement = data; assert(!result->status);
    db_sync_instance_remove(si_find(replacement->inst, replacement->channel));
    struct DB_SYNC_INSTANCE* si = db_sync_instance_activate(replacement->inst, &replacement->prepared); assert(si);
    db_sync_set_insert_cb(si, on_msg_inserted, replacement->inst);
}
struct bootstrap_job {
    struct chat_core_bootstrap_input input;
    struct chat_core_bootstrap* prepared;
    struct chat_sync_catalog* catalog;
    struct UTUN_INSTANCE* inst; /* Только done. */
    int calls, expected;
    char name[64], path[512];
};
static int bootstrap_work(sqlite3* db, void* data) {
    struct bootstrap_job* job = data;
    int rc = chat_core_prepare(db, &job->input, &job->prepared);
    return rc == SQLITE_OK ? chat_sync_catalog_read(db, &job->catalog) : rc;
}
static void bootstrap_done(void* data, const struct db_async_result* result) {
    struct bootstrap_job* job = data; assert(!job->calls++ && result->sqlite_code == job->expected);
    if (job->expected) { assert(result->status && !job->inst->chat_core); return; }
    assert(!result->status && job->prepared && job->catalog && !chat_core_activate(job->inst, job->prepared));
    assert(chat_core_media_base(job->inst) && chat_core_is_initialized(job->inst));
    unsigned before = ready_events; chat_core_publish_ready(job->inst); chat_core_publish_ready(job->inst);
    assert(ready_events == before + 1);
}
static void media_saved(void* data, int success) { assert(*(int*)data == -1); *(int*)data = success; }
static int manual_probe(sqlite3* db, void* data) {
    struct attrs_probe* probe = data; sqlite3_stmt* st = NULL; char sql[256];
    snprintf(sql, sizeof(sql), "SELECT message_id FROM msg_%s WHERE json_extract(CAST(data AS TEXT),'$.ct')='application/octet-stream'", probe->channel);
    assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32);
    memcpy(probe->message, sqlite3_column_blob(st, 0), 32); int rc = sqlite3_finalize(st); return rc ? rc : attrs_probe_work(db, probe);
}
struct media_recovery_fixture {
    const char* channel;
    const char* base;
    char path[MEDIA_PATH_MAX], filename[256];
};

static int unindex_media(sqlite3* db, void* data) {
    struct media_recovery_fixture* fixture = data;
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT json_extract(CAST(data AS TEXT),'$.d') FROM msg_%s WHERE json_extract(CAST(data AS TEXT),'$.ct')='application/octet-stream'",
        fixture->channel);
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    const char* body = (const char*)sqlite3_column_text(st, 0);
    struct media_index_result manifest;
    assert(!attachment_manifest_parse(body, &manifest));
    assert(!attachment_group_filename(manifest.media_id, body, "application/octet-stream", fixture->filename));
    media_index_result_free(&manifest);
    assert(!sqlite3_finalize(st));
    char relative[MEDIA_PATH_MAX];
    snprintf(relative, sizeof(relative), "media/%s/%s", fixture->channel, fixture->filename);
    assert(!media_index_path(fixture->base, relative, fixture->path));
    assert(!sqlite3_exec(db, "DELETE FROM media_files", NULL, NULL, NULL) && sqlite3_changes(db) == 1);
    return media_index_track_preparation_write(db, fixture->channel, fixture->path, fixture->base);
}

static int recovered_media(sqlite3* db, void* data) {
    (void)data;
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT (SELECT count(*) FROM media_files),(SELECT count(*) FROM media_preparations)", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1 && !sqlite3_column_int(st, 1));
    return sqlite3_finalize(st);
}

static int media_probe(sqlite3* db, void* data) {
    const char* channel = data; char sql[512]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT json_extract(local_attrs,'$.st'),json_extract(local_attrs,'$.fp') FROM msg_%s"
        " WHERE json_extract(CAST(data AS TEXT),'$.ct')='application/octet-stream'", channel);
    assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    assert(!strcmp((const char*)sqlite3_column_text(st, 0), "fl") && strlen((const char*)sqlite3_column_text(st, 1)) == 36 && !strcmp((const char*)sqlite3_column_text(st, 1) + 32, ".bin"));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    snprintf(sql, sizeof(sql), "DELETE FROM msg_%s WHERE json_extract(CAST(data AS TEXT),'$.ct')='application/octet-stream'", channel);
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}
static void media_case(struct UTUN_INSTANCE* inst, const char* channel, const char* directory) {
    assert(!db_async_destroy(inst->db_worker)); char database[1200]; snprintf(database, sizeof(database), "%s/chats.db", directory);
    int opened = 0; inst->db_worker = db_async_create(inst->ua, database, 16, ready, &opened); assert(inst->db_worker); drain(inst); assert(opened);
    char source[1200], folder[1200], target[1400];
    snprintf(source, sizeof(source), "%s/source.bin", directory); FILE* file = fopen(source, "wb"); assert(file);
    assert(fwrite("media worker body", 1, 17, file) == 17 && !fclose(file));
    snprintf(folder, sizeof(folder), "%s/media", chat_core_media_base(inst)); assert(!utun_mkdir(folder, 0700) || errno == EEXIST);
    snprintf(folder, sizeof(folder), "%s/media/%s", chat_core_media_base(inst), channel); assert(!utun_mkdir(folder, 0700) || errno == EEXIST);
    snprintf(target, sizeof(target), "%s/worker-file.bin", folder);
    struct chat_msg_submit request = {.inst=inst,.data=(uint8_t*)"test.bin",.data_len=8,.media_copy=1};
    strcpy(request.channel_id, channel); strcpy(request.content_type, "application/octet-stream");
    assert(strlen(source) < sizeof(request.media_src) && strlen(target) < sizeof(request.media_dest));
    strcpy(request.media_src, source); strcpy(request.media_dest, target); int success = -1;
    chat_core_submit_media_message(inst, &request, media_saved, &success); memset(request.media_src, 'x', sizeof(request.media_src));
    uint64_t deadline = get_time_us() + 5000000;
    while (success < 0 && get_time_us() < deadline) uasync_poll(inst->ua, 1);
    assert(success == 1); drain(inst);
    struct attrs_probe manual = {0}; strcpy(manual.channel, channel);
    db_async_submit(inst->db_worker, "manual media probe", DB_ASYNC_READ, manual_probe, &manual, probe_done); drain(inst);
    struct media_recovery_fixture recovery = {.channel=channel, .base=chat_core_media_base(inst)};
    db_async_submit(inst->db_worker, "unindex media", DB_ASYNC_WRITE, unindex_media, &recovery, probe_done); drain(inst);
    FILE* wrong = fopen(recovery.path, "wb"); assert(wrong && fwrite("wrong worker body", 1, 17, wrong) == 17 && !fclose(wrong));
    unsigned events = download_events; capture_download = 1;
    chat_core_attachment_download(inst, channel, manual.id);
    for (unsigned poll = 0; poll < 500 && !manual_done; poll++) uasync_poll(inst->ua, 1);
    assert(!capture_download && manual_done && download_events == events && !strcmp(manual_path, recovery.path));
    void (*complete)(void*,int) = manual_done; void* complete_arg = manual_arg; manual_done = NULL; manual_arg = NULL;
    complete(complete_arg, -1); assert(download_events == events); drain(inst); assert(download_events == ++events);
    assert(!file_io_publish(target, recovery.path));
    capture_download = 1;
    chat_core_attachment_download(inst, channel, manual.id);
    for (unsigned poll = 0; poll < 500 && download_events == events; poll++) uasync_poll(inst->ua, 1);
    assert(capture_download && !manual_done && download_events == events + 1);
    capture_download = 0;
    db_async_submit(inst->db_worker, "verify recovered media", DB_ASYNC_READ, recovered_media, NULL, probe_done); drain(inst);
    db_async_submit(inst->db_worker, "manual attrs probe", DB_ASYNC_READ, attrs_probe_work, &manual, probe_done); drain(inst);
    char expected_attrs[384];
    snprintf(expected_attrs, sizeof(expected_attrs), "{\"st\":\"fl\",\"fp\":\"%s\"}", recovery.filename);
    assert(!strcmp(manual.json, expected_attrs));
    chat_media_backfill(inst); chat_media_autodownload_backfill(inst); chat_media_cache_enforce(inst);
    assert(CC(inst)->media_scan && CC(inst)->media_scan_pending); drain(inst);
    assert(!CC(inst)->media_scan && !CC(inst)->media_scan_pending && CC(inst)->media_cache_bytes == 17);
    __atomic_store_n(&hold_scan, 1, __ATOMIC_SEQ_CST); chat_media_backfill(inst);
    deadline = get_time_us() + 5000000;
    while (!__atomic_load_n(&scan_entered, __ATOMIC_SEQ_CST) && get_time_us() < deadline) uasync_poll(inst->ua, 1);
    assert(__atomic_load_n(&scan_entered, __ATOMIC_SEQ_CST)); void* previous_scan = CC(inst)->media_scan;
    chat_media_startup_backfill_destroy(inst); assert(!CC(inst)->media_scan && !chat_media_startup_backfill_init(inst));
    chat_media_cache_enforce(inst); assert(CC(inst)->media_scan && CC(inst)->media_scan != previous_scan);
    pthread_mutex_lock(&preflight_mutex); __atomic_store_n(&hold_scan, 0, __ATOMIC_SEQ_CST);
    pthread_cond_signal(&preflight_condition); pthread_mutex_unlock(&preflight_mutex); drain(inst);
    assert(!CC(inst)->media_scan && CC(inst)->media_cache_bytes == 17);
    fail_alloc = 3; chat_media_backfill(inst); assert(!fail_alloc && !CC(inst)->media_scan);
    fault = 14; chat_media_backfill(inst); drain(inst); assert(!fault && !CC(inst)->media_scan && CC(inst)->media_cache_bytes == 17);
    fault = 15; chat_media_backfill(inst); drain(inst); assert(!fault && !CC(inst)->media_scan);
    chat_media_backfill(inst); assert(!CC(inst)->media_scan);
    assert(!db_async_destroy(inst->db_worker)); opened = 0;
    inst->db_worker = db_async_create(inst->ua, database, 16, ready, &opened); assert(inst->db_worker); drain(inst); assert(opened);
    uint64_t limit = inst->config->global.chatserver_storage_total_size; inst->config->global.chatserver_storage_total_size = 1;
    chat_media_cache_enforce(inst); drain(inst); assert(!CC(inst)->media_cache_bytes && ma_file_size(manual_path) < 0);
    inst->config->global.chatserver_storage_total_size = limit;
    db_async_submit(inst->db_worker, "media-probe", DB_ASYNC_WRITE, media_probe, (void*)channel, probe_done); drain(inst);
    assert(!test_unlink(source) && !test_rmdir(folder));
    snprintf(folder, sizeof(folder), "%s/media", chat_core_media_base(inst)); assert(!test_rmdir(folder));
    puts("PASS real FILE media submit: queued index/file worker/message WRITE, copied source and atomic local path, no owner SQL");
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(1); owner = pthread_self();
    size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    char directory[1024], config[1600], path[1200]; const char* tmp = getenv("TMPDIR");
    snprintf(directory, sizeof(directory), "%s/utun-message-XXXXXX", tmp && *tmp ? tmp : "/tmp"); assert(!test_mkdtemp(directory));
    snprintf(config, sizeof(config), "[global]\nmy_node_name=message-worker\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst)); chat_core_create_channel_auto(inst, "worker channel");
    sqlite3* legacy = inst->topo_sqlite_db; sqlite3_stmt* st = NULL; char channel[65];
    assert(!sqlite3_prepare_v2(legacy, "SELECT channel_id FROM channels", -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0)); assert(!sqlite3_finalize(st));
    struct chat_msg_submit legacy_req = {.inst=inst, .data=(uint8_t*)"legacy", .data_len=6};
    snprintf(legacy_req.channel_id, sizeof(legacy_req.channel_id), "%s", channel); strcpy(legacy_req.content_type, "text");
    assert(!sqlite3_exec(legacy, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    assert(!sqlite3_exec(legacy, "INSERT INTO ui_state(key,value) VALUES('outer-sentinel','kept')", NULL, NULL, NULL));
    struct reply legacy_reply = {.expected=-1, .expected_code=SQLITE_MISUSE};
    chat_core_submit_message_async(inst, &legacy_req, saved, &legacy_reply);
    assert(legacy_reply.calls == 1 && !legacy_reply.result.committed && !sqlite3_get_autocommit(legacy));
    assert(!sqlite3_prepare_v2(legacy, "SELECT value FROM ui_state WHERE key='outer-sentinel'", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "kept"));
    assert(!sqlite3_finalize(st) && !sqlite3_exec(legacy, "ROLLBACK", NULL, NULL, NULL)); st = NULL;
    legacy_reply = (struct reply){0}; chat_core_submit_message_async(inst, &legacy_req, saved, &legacy_reply);
    assert(legacy_reply.calls == 1 && legacy_reply.result.committed && legacy_reply.result.timestamp && legacy_reply.result.ver == 2);
    legacy_req.expected_ver = legacy_reply.result.ver; memcpy(legacy_req.message_id, legacy_reply.result.message_id, 32);
    legacy_req.data = (uint8_t*)"legacy edit"; legacy_req.data_len = 11; uint64_t legacy_timestamp = legacy_reply.result.timestamp;
    legacy_reply = (struct reply){0}; chat_core_submit_message_async(inst, &legacy_req, saved, &legacy_reply);
    assert(legacy_reply.calls == 1 && legacy_reply.result.ver == 3 && legacy_reply.result.timestamp == legacy_timestamp &&
        !memcmp(legacy_reply.result.message_id, legacy_req.message_id, 32));
    legacy_req.expected_ver=legacy_reply.result.ver; legacy_req.delete_message=1; legacy_req.data=NULL; legacy_req.data_len=0;
    legacy_reply=(struct reply){0}; chat_core_submit_message_async(inst,&legacy_req,saved,&legacy_reply);
    assert(legacy_reply.calls==1 && legacy_reply.result.committed && legacy_reply.result.ver==4);
    char deleted_sql[256]; snprintf(deleted_sql,sizeof(deleted_sql),
        "SELECT json_extract(CAST(data AS TEXT),'$.deleted'),length(json_extract(CAST(data AS TEXT),'$.d')) FROM msg_%s",channel);
    assert(!sqlite3_prepare_v2(legacy,deleted_sql,-1,&st,NULL) && sqlite3_step(st)==SQLITE_ROW);
    assert(sqlite3_column_int(st,0)==1 && sqlite3_column_int(st,1)==0); assert(!sqlite3_finalize(st)); st=NULL;
    char sql[512], table[80]; snprintf(table, sizeof(table), "msg_%s", channel);
    snprintf(sql, sizeof(sql), "DELETE FROM %s;DELETE FROM chat_heads;DELETE FROM chat_seals;DELETE FROM chat_effects;DELETE FROM member_faults;DELETE FROM member_log;DELETE FROM member_chains;"
        "DELETE FROM merkle_tree_hash WHERE namespace GLOB '4:*'", table); assert(!sqlite3_exec(legacy, sql, NULL, NULL, NULL));
    assert(!db_sync_verify(si_find(inst, channel))); chat_event_set_handler(inst, event);
    struct replacement replacement = {.inst=inst}; snprintf(replacement.channel, sizeof(replacement.channel), "%s", channel);
    assert(!db_sync_instance_prepare(legacy, table, channel, &replacement.prepared));
    struct db_async* reader = inst->db_worker; inst->db_worker = NULL; assert(!db_async_destroy(reader));
    snprintf(path, sizeof(path), "%s", sqlite3_db_filename(legacy, "main")); inst->topo_sqlite_db = CC(inst)->db = NULL;
    forbid_owner_sql = 1; int opened = 0; inst->db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst->db_worker);
    drain(inst); assert(opened);
    char nickname[] = "copied nickname", owner_json[] = "{\"worker_note\":\"copied note\"}";
    struct member_reply member = {0};
    chat_core_save_member_async(inst, channel, inst->my_ed25519_pubkey, nickname, owner_json, member_saved, &member);
    assert(!member.calls); memset(nickname, 'x', strlen(nickname)); memset(owner_json, 'x', strlen(owner_json));
    drain(inst); assert(member.calls == 1);
    struct member_probe probe = {.node=inst->node_id}; strcpy(probe.channel, channel);
    db_async_submit(inst->db_worker, "member-probe", DB_ASYNC_READ, member_probe_work, &probe, probe_done); drain(inst);
    fail_alloc = 2; member = (struct member_reply){.status=DB_ASYNC_NOMEM,.code=SQLITE_NOMEM};
    chat_core_save_member_async(inst, channel, inst->my_ed25519_pubkey, "out of memory", NULL, member_saved, &member);
    assert(member.calls == 1 && !fail_alloc);
    char stale[96]; snprintf(stale, sizeof(stale), "{\"ver\":\"%d\",\"worker_note\":\"stale\"}", probe.version - 1);
    member = (struct member_reply){.status=DB_ASYNC_SQL,.code=SQLITE_BUSY};
    chat_core_save_member_async(inst, channel, inst->my_ed25519_pubkey, "must rollback", stale, member_saved, &member);
    drain(inst); assert(member.calls == 1);
    fault = 7; member = (struct member_reply){.status=DB_ASYNC_SQL,.code=SQLITE_BUSY};
    chat_core_save_member_async(inst, channel, inst->my_ed25519_pubkey, "must rollback", "{\"worker_note\":\"retry\"}", member_saved, &member);
    drain(inst); assert(member.calls == 1 && !fault);
    struct db_async_task stop_member = {.name="member-stop",.mode=DB_ASYNC_READ,.work=never,.guard=cancel,.done=stop_groups,.data=inst};
    db_async_run(inst->db_worker, &stop_member); member = (struct member_reply){.status=DB_ASYNC_CANCELLED,.code=SQLITE_ABORT};
    chat_core_save_member_async(inst, channel, inst->my_ed25519_pubkey, "must cancel", NULL, member_saved, &member);
    drain(inst); assert(member.calls == 1); inst->group_started = 1;
    db_async_submit(inst->db_worker, "member-probe", DB_ASYNC_READ, member_probe_work, &probe, probe_done); drain(inst);
    char text[] = "quoted \"текст\"\nline";
    struct chat_msg_submit req = {.inst=inst, .data=(uint8_t*)text, .data_len=(uint32_t)strlen(text)};
    snprintf(req.channel_id, sizeof(req.channel_id), "%s", channel); snprintf(req.content_type, sizeof(req.content_type), "text");
    struct reply reply = {0}; chat_core_submit_message_async(inst, &req, saved, &reply); assert(!reply.calls);
    memset(text, 'x', strlen(text)); drain(inst); assert(reply.calls == 1 && reply.result.committed && reply.result.ver == 1 && received == 1);
    uint8_t message[32]; memcpy(message, reply.result.message_id, 32);
    req.expected_ver = 1; memcpy(req.message_id, message, 32); req.data = (uint8_t*)"edited"; req.data_len = 6;
    reply = (struct reply){0}; chat_core_submit_message_async(inst, &req, saved, &reply); drain(inst);
    assert(reply.calls == 1 && reply.result.ver == 2 && updated == 1 && !memcmp(message, reply.result.message_id, 32));
    reply = (struct reply){.expected=-2}; chat_core_submit_message_async(inst, &req, saved, &reply); drain(inst);
    assert(reply.calls == 1 && !reply.result.committed && updated == 1);
    req.expected_ver = 2; reply = (struct reply){0}; chat_core_submit_message_async(inst, &req, saved, &reply); drain(inst);
    assert(reply.calls == 1 && reply.result.ver == 2 && updated == 1);
    struct attrs_probe attrs = {0}; strcpy(attrs.channel, channel); memcpy(attrs.message, message, 32);
    db_async_submit(inst->db_worker, "attrs probe", DB_ASYNC_READ, attrs_probe_work, &attrs, probe_done); drain(inst);
    char local_json[] = "{\"worker\":\"copied\"}";
    assert(!chat_core_update_local_attrs(inst, channel, attrs.timestamp, attrs.signature, local_json)); memset(local_json, 'x', strlen(local_json)); drain(inst);
    chat_core_mark_voice_played(inst, channel, attrs.message); drain(inst);
    db_async_submit(inst->db_worker, "attrs probe", DB_ASYNC_READ, attrs_probe_work, &attrs, probe_done); drain(inst);
    assert(!strcmp(attrs.json, "{\"worker\":\"copied\",\"pl\":1}"));
    for (int point = 0; point < 2; point++) {
        fault = point ? 2 : 9; assert(!chat_core_update_local_attrs(inst, channel, attrs.timestamp, attrs.signature, "{}")); drain(inst); assert(!fault);
        db_async_submit(inst->db_worker, "attrs probe", DB_ASYNC_READ, attrs_probe_work, &attrs, probe_done); drain(inst);
        assert(!strcmp(attrs.json, "{\"worker\":\"copied\",\"pl\":1}"));
    }
    fail_alloc = 3; assert(chat_core_update_local_attrs(inst, channel, attrs.timestamp, attrs.signature, "{}") == -1 && !fail_alloc);
    feedback_cases(inst, channel, message, path); unsigned message_events = received;
    req.expected_ver = 0;
    for (int point = 1; point <= 2; point++) {
        fault = point; reply = (struct reply){.expected=-1, .expected_code=SQLITE_BUSY};
        chat_core_submit_message_async(inst, &req, saved, &reply); drain(inst);
        assert(reply.calls == 1 && !reply.result.committed && !fault && received == message_events);
    }
    fail_alloc = 1; reply = (struct reply){.expected=-1, .expected_code=SQLITE_NOMEM};
    chat_core_submit_message_async(inst, &req, saved, &reply); assert(reply.calls == 1 && !fail_alloc);
    struct db_async_task barrier = {.name="message-generation", .mode=DB_ASYNC_READ, .work=never, .guard=cancel,
        .done=generation, .data=inst};
    /* Начало chat_service_stop: content уже выключен, но cc/SI ещё живы во время drain. */
    inst->chat_content_started = 0; reply = (struct reply){.expected=-1, .expected_code=SQLITE_MISUSE};
    chat_core_submit_message_async(inst, &req, saved, &reply); assert(reply.calls == 1 && !reply.result.committed);
    inst->chat_content_started = 1; inst->destroying = 1; reply = (struct reply){.expected=-1, .expected_code=SQLITE_MISUSE};
    chat_core_submit_message_async(inst, &req, saved, &reply); assert(reply.calls == 1 && !reply.result.committed); inst->destroying = 0;
    barrier.done = stop_content; db_async_run(inst->db_worker, &barrier); reply = (struct reply){.expected=-1, .expected_code=SQLITE_ABORT};
    chat_core_submit_message_async(inst, &req, saved, &reply); drain(inst); assert(reply.calls == 1 && !reply.result.committed);
    inst->chat_content_started = 1; req.expected_ver = 2; __atomic_store_n(&hold_edit, 1, __ATOMIC_SEQ_CST);
    reply = (struct reply){.expected=-1}; chat_core_submit_message_async(inst, &req, saved, &reply);
    uint64_t edit_deadline = get_time_us() + 5000000;
    while (!__atomic_load_n(&edit_entered, __ATOMIC_SEQ_CST) && get_time_us() < edit_deadline) uasync_poll(ua, 1);
    assert(__atomic_load_n(&edit_entered, __ATOMIC_SEQ_CST)); inst->chat_content_started = 0;
    pthread_mutex_lock(&preflight_mutex); __atomic_store_n(&hold_edit, 0, __ATOMIC_SEQ_CST);
    pthread_cond_signal(&preflight_condition); pthread_mutex_unlock(&preflight_mutex); drain(inst);
    assert(reply.calls == 1 && reply.result.committed && received == message_events && updated == 1);
    inst->chat_content_started = 1; req.expected_ver = 0; barrier.done = generation;
    db_async_run(inst->db_worker, &barrier); reply = (struct reply){.expected=-1, .expected_code=SQLITE_ABORT};
    chat_core_submit_message_async(inst, &req, saved, &reply); drain(inst); assert(reply.calls == 1 && !reply.result.committed);
    barrier.done = replace_history; barrier.data = &replacement; barrier.work = prepare_history; barrier.guard = NULL; barrier.mode = DB_ASYNC_WRITE;
    __atomic_store_n(&hold_preflight, 1, __ATOMIC_SEQ_CST);
    db_async_run(inst->db_worker, &barrier); reply = (struct reply){.expected=-1, .expected_code=SQLITE_ABORT};
    chat_core_submit_message_async(inst, &req, saved, &reply);
    uint64_t until = get_time_us() + 5000000;
    while ((!reply.calls || !__atomic_load_n(&preflight_entered, __ATOMIC_SEQ_CST)) && get_time_us() < until) uasync_poll(ua, 1);
    assert(reply.calls == 1 && !reply.result.committed && __atomic_load_n(&preflight_entered, __ATOMIC_SEQ_CST));
    int resets = __atomic_load_n(&preflight_resets, __ATOMIC_SEQ_CST); assert(resets == 1);
    for (int i = 0; i < 3; i++) {
        assert(retry_timer && retry_callback); uasync_cancel_timeout(ua, retry_timer); retry_callback(retry_arg);
    }
    pthread_mutex_lock(&preflight_mutex); __atomic_store_n(&hold_preflight, 0, __ATOMIC_SEQ_CST);
    pthread_cond_signal(&preflight_condition); pthread_mutex_unlock(&preflight_mutex);
    while (!db_sync_index_ready(si_find(inst, channel)) && get_time_us() < until) uasync_poll(ua, 1);
    assert(db_sync_index_ready(si_find(inst, channel))); drain(inst);
    assert(__atomic_load_n(&preflight_resets, __ATOMIC_SEQ_CST) == resets);
    assert(!db_async_destroy(inst->db_worker)); inst->db_worker = NULL;
    opened = 0; inst->db_worker = db_async_create(ua, path, 1, ready, &opened); assert(inst->db_worker); drain(inst); assert(opened);
    barrier.done = ignore; barrier.guard = cancel; barrier.work = never;
    db_async_run(inst->db_worker, &barrier); reply = (struct reply){.expected=-1};
    chat_core_submit_message_async(inst, &req, saved, &reply); assert(reply.calls == 1 && !reply.result.committed); drain(inst);
    db_async_run(inst->db_worker, &barrier); member = (struct member_reply){.status=DB_ASYNC_FULL};
    chat_core_save_member_async(inst, channel, inst->my_ed25519_pubkey, "queue full", NULL, member_saved, &member);
    assert(member.calls == 1); drain(inst);
    fault = 3; reply = (struct reply){.expected=-1, .expected_code=SQLITE_IOERR};
    chat_core_submit_message_async(inst, &req, saved, &reply); drain(inst); assert(reply.calls == 1 && !fault);
    reply = (struct reply){.expected=-1}; chat_core_submit_message_async(inst, &req, saved, &reply);
    drain(inst); assert(reply.calls == 1 && !reply.result.committed);
    assert(!db_async_destroy(inst->db_worker)); inst->db_worker = NULL;
    opened = 0; inst->db_worker = db_async_create(ua, path, 1, ready, &opened); assert(inst->db_worker); drain(inst); assert(opened);
    for (int point = 4; point <= 5; point++) {
        fault = point; reply = (struct reply){.expected=-1, .expected_code=point == 4 ? SQLITE_IOERR : SQLITE_CORRUPT};
        chat_core_submit_message_async(inst, &req, saved, &reply); drain(inst); assert(reply.calls == 1 && !fault);
        reply = (struct reply){.expected=-1}; chat_core_submit_message_async(inst, &req, saved, &reply);
        drain(inst); assert(reply.calls == 1 && !reply.result.committed);
        assert(!db_async_destroy(inst->db_worker)); inst->db_worker = NULL;
        opened = 0; inst->db_worker = db_async_create(ua, path, 1, ready, &opened); assert(inst->db_worker); drain(inst); assert(opened);
    }
    fault = 8; member = (struct member_reply){.status=DB_ASYNC_SQL,.code=SQLITE_IOERR};
    chat_core_save_member_async(inst, channel, inst->my_ed25519_pubkey, "failed IO", "{\"worker_note\":\"failed IO\"}", member_saved, &member);
    drain(inst); assert(member.calls == 1 && !fault);
    member = (struct member_reply){.status=DB_ASYNC_CLOSED,.code=SQLITE_IOERR};
    chat_core_save_member_async(inst, channel, inst->my_ed25519_pubkey, "closed IO", NULL, member_saved, &member);
    drain(inst); assert(member.calls == 1);
    assert(!db_async_destroy(inst->db_worker)); inst->db_worker = NULL;
    opened = 0; inst->db_worker = db_async_create(ua, path, 1, ready, &opened); assert(inst->db_worker); drain(inst); assert(opened);
    db_async_submit(inst->db_worker, "member-probe", DB_ASYNC_READ, member_probe_work, &probe, probe_done); drain(inst);
    media_case(inst, channel, directory);
    forbid_owner_sql = 0; inst->topo_sqlite_db = CC(inst)->db = legacy; group_service_stop(inst);
    inst->topo_sqlite_db = NULL; forbid_owner_sql = 1; chat_event_set_handler(inst, event);
    struct bootstrap_job bootstrap = {.inst=inst, .expected=SQLITE_BUSY,
        .input={.node_id=inst->node_id, .now_sec=(uint64_t)time(NULL)}};
    snprintf(bootstrap.name, sizeof(bootstrap.name), "%s", inst->name); snprintf(bootstrap.path, sizeof(bootstrap.path), "%s", path);
    bootstrap.input.name = bootstrap.name; bootstrap.input.db_path = bootstrap.path;
    memcpy(bootstrap.input.x25519, inst->my_keys.public_key, 32); memcpy(bootstrap.input.ed25519, inst->my_ed25519_pubkey, 32);
    struct db_async_task prepare = {.name="chat-bootstrap-test", .mode=DB_ASYNC_WRITE, .data=&bootstrap, .work=bootstrap_work, .done=bootstrap_done};
    fault = 6; db_async_run(inst->db_worker, &prepare); drain(inst); assert(bootstrap.calls == 1 && !fault);
    assert(!bootstrap.prepared && !bootstrap.catalog);
    bootstrap.calls = 0; bootstrap.expected = SQLITE_IOERR; __atomic_store_n(&base_finalize_fault, 2, __ATOMIC_SEQ_CST);
    db_async_run(inst->db_worker, &prepare); drain(inst); assert(bootstrap.calls == 1 && !base_finalize_fault);
    bootstrap.calls = 0; db_async_run(inst->db_worker, &prepare); assert(bootstrap.calls == 1 && !bootstrap.prepared);
    assert(!db_async_destroy(inst->db_worker)); opened = 0; inst->db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst->db_worker);
    drain(inst); assert(opened); bootstrap.calls = bootstrap.expected = 0;
    uint64_t previous_generation = inst->group_generation;
    db_async_run(inst->db_worker, &prepare); drain(inst); assert(bootstrap.calls == 1 && inst->group_generation > previous_generation);
    chat_sync_catalog_free(bootstrap.catalog); chat_core_bootstrap_free(bootstrap.prepared);
    chat_core_quiesce(inst); chat_core_quiesce(inst);
    assert(inst->chat_core && !chat_core_is_initialized(inst) && !chat_core_generation(inst));
    chat_core_destroy(inst); assert(!inst->chat_core);
    assert(!db_async_destroy(inst->db_worker)); inst->db_worker = NULL; forbid_owner_sql = 0;
    inst->topo_sqlite_db = legacy;
    snprintf(sql, sizeof(sql), "SELECT count(*),json_extract(CAST(data AS TEXT),'$.d'),ver FROM msg_%s", channel);
    assert(!sqlite3_prepare_v2(legacy, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 1 && !strcmp((const char*)sqlite3_column_text(st, 1), "edited") && sqlite3_column_int(st, 2) == 2);
    assert(!sqlite3_finalize(st)); utun_instance_destroy(inst); uasync_destroy(ua, 0); assert(!db_storage_clear(directory));
    const char* subdirs[] = {"member_files/.uploads", "member_files", "dm_media", "dm_pending"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, subdirs[i]); assert(!test_rmdir(path));
    }
    assert(!test_rmdir(directory)); if (u_get_allocated_count() != baseline) u_report_unfreed_blocks();
    assert(u_get_allocated_count() == baseline);
    assert(!pthread_cond_destroy(&preflight_condition) && !pthread_mutex_destroy(&preflight_mutex));
    puts("PASS message worker: real notifications, copied inputs, send/edit/stale/noop, SQL/commit/OOM/FULL, generation and reopen"); return 0;
}
