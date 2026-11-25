/* Пути, атомарность SQLite, повтор удаления и поздние completion исходящих медиа. */
#include "../lib/platform_compat.h"
#include "../lib/file_io.h"
#include "../lib/debug_config.h"
#include "../src/media_delivery/media_index.h"
#include "../src/media_delivery/media_download.h"
#include "../src/media_async/media_async.h"
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_event.h"
#include "test_utils.h"
#include "test_membership.h"
#include "media_members_fixture.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>

static char root[256] = "/tmp/media_index_regressions_XXXXXX";
static uint8_t key[32] = {1}, ids[32] = {1};
static struct media_index_result result = { .media_id = {2}, .content_hash = {3},
    .file_size = 14, .block_size = 7, .num_blocks = 2, .block_ids = ids };

#ifdef __linux__
int __real_ma_sha256_file(const char*, uint8_t[32]);
int __wrap_ma_sha256_file(const char* path, uint8_t hash[32]) {
    if (strstr(path, "/hash-failure")) {
        memset(hash, 0, 32); DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "injected hash failure after file copy path=%s", path); return -1;
    }
    return __real_ma_sha256_file(path, hash);
}
#endif

static void sql(sqlite3* db, const char* query) { assert(sqlite3_exec(db, query, NULL, NULL, NULL) == SQLITE_OK); }
static int scalar(sqlite3* db, const char* query) {
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, query, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); int value = sqlite3_column_int(st, 0);
    assert(sqlite3_finalize(st) == SQLITE_OK); return value;
}
static sqlite3* database(void) {
    sqlite3* db = NULL; assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(!media_index_init(db)); return db;
}
static void write_file(const char* path) {
    FILE* file = fopen(path, "wb"); assert(file && fwrite("source-payload", 1, 14, file) == 14 && !fclose(file));
}
static int commit(sqlite3* db, const char* channel, const char* path) {
    return media_index_commit(db, &result, (const uint8_t[32]){1}, key, channel, path, root);
}

static void paths(void) {
    char relative[MEDIA_PATH_MAX], path[MEDIA_PATH_MAX], base[MEDIA_BASE_MAX];
    assert(!media_index_location("/root///a/./file", "/root///", relative) && !strcmp(relative, "a/file"));
    assert(!media_index_path("/", "a/file", path) && !strcmp(path, "/a/file"));
    assert(!media_index_location("/a/file", "/", relative) && !strcmp(relative, "a/file"));
    assert(!media_index_location("./a/file", ".", relative) && !strcmp(relative, "a/file"));
    assert(media_index_location("/root2/file", "/root", relative) == -1);
    assert(media_index_location("/root/../file", "/root", relative) == -1);
    assert(media_index_location("/root", "/root", relative) == -1);
    assert(media_index_path(root, "../file", path) == -1 && media_index_path(root, "/file", path) == -1);
    assert(media_index_path(root, "a//file", path) == -1 && media_index_path(root, "a/./file", path) == -1);
    assert(media_index_path(root, "C:/file", path) == -1 && media_index_path(root, "a\\file", path) == -1);
    memset(relative, 'a', sizeof(relative)); relative[sizeof(relative) - 1] = 0;
    assert(!media_index_path(".", relative, path) && strlen(path) == MEDIA_PATH_MAX - 1);
    assert(!media_index_location(path, ".", relative) && strlen(relative) == MEDIA_PATH_MAX - 1);
    assert(!media_index_path(".", relative, relative) && strlen(relative) == MEDIA_PATH_MAX - 1);
    assert(media_index_path("/", relative, path) == -1);
    relative[sizeof(relative) - 2] = 0;
    assert(!media_index_path("/", relative, path) && strlen(path) == MEDIA_PATH_MAX - 1);
    sqlite3* db = database();
    assert(!media_index_set_base(db, "/root///") && !media_index_set_base(db, "/root/./"));
    assert(!media_index_get_base(db, base) && !strcmp(base, "/root"));
    assert(media_index_set_base(db, "/other") == -1);
    assert(!media_index_get_base(db, base) && !strcmp(base, "/root")); sqlite3_close(db);
#ifdef _WIN32
    assert(!media_index_location("C:\\root\\a", "C:/root/", relative) && !strcmp(relative, "a"));
    assert(!media_index_path("C:/", "a", path) && !strcmp(path, "C:/a"));
    assert(!media_index_location("//server/share/a", "//server/share/", relative) && !strcmp(relative, "a"));
#endif
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "media regression paths/base boundaries PASS");
}

struct denied { const char* index; int release, read_schema; };
static int authorizer(void* arg, int action, const char* one, const char* two, const char* db, const char* trigger) {
    (void)db; (void)trigger; struct denied* denied = arg;
    if (action == SQLITE_CREATE_INDEX && denied->index && !strcmp(one, denied->index)) return SQLITE_DENY;
    if (action == SQLITE_READ && denied->read_schema && !strcmp(one, "sqlite_master")) return SQLITE_DENY;
    if (action == SQLITE_SAVEPOINT && denied->release && !strcmp(one, "RELEASE") && !strcmp(two, "media_registration")) {
        denied->release = 0; return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static void transactions(void) {
    const char* indices[] = {"block_id", "media_id", "chat_id", "holder", "content_hash"};
    for (unsigned i = 0; i < sizeof(indices) / sizeof(*indices); i++) {
        sqlite3* db = NULL; assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
        char name[80]; snprintf(name, sizeof(name), "idx_media_files_%s", indices[i]);
        struct denied denied = {.index = name}; assert(!sqlite3_set_authorizer(db, authorizer, &denied));
        assert(media_index_init(db) == -1);
        assert(!sqlite3_set_authorizer(db, NULL, NULL) && !media_index_init(db)); sqlite3_close(db);
    }
    char path[MEDIA_PATH_MAX]; assert(!media_index_path(root, "atomic", path));
    sqlite3* db = database();
    sql(db, "CREATE TABLE marker(n); BEGIN; INSERT INTO marker VALUES(42)");
    sql(db, "CREATE TRIGGER fail_second BEFORE INSERT ON media_files WHEN NEW.chunk=1 BEGIN SELECT RAISE(ABORT,'second'); END");
    assert(commit(db, "1", path) == -1 && scalar(db, "SELECT count(*) FROM media_files") == 0);
    assert(!sqlite3_get_autocommit(db) && scalar(db, "SELECT n FROM marker") == 42);
    assert(media_index_commit(db, &result, (const uint8_t[32]){1}, NULL, "1", path, root) == -1);
    assert(!scalar(db, "SELECT count(*) FROM media_files"));
    sql(db, "DROP TRIGGER fail_second");
    assert(!commit(db, "1", path) && scalar(db, "SELECT count(*) FROM media_files") == 2);
    assert(!sqlite3_get_autocommit(db)); sql(db, "ROLLBACK");
    assert(!scalar(db, "SELECT count(*) FROM media_files"));
    struct denied denied = {.release = 1}; assert(!sqlite3_set_authorizer(db, authorizer, &denied));
    assert(commit(db, "1", path) == -1 && !scalar(db, "SELECT count(*) FROM media_files"));
    assert(sqlite3_get_autocommit(db)); assert(!sqlite3_set_authorizer(db, NULL, NULL));
    sql(db, "CREATE TRIGGER rollback_all BEFORE INSERT ON media_files BEGIN SELECT RAISE(ROLLBACK,'all'); END; BEGIN");
    assert(commit(db, "1", path) == -1 && sqlite3_get_autocommit(db));
    sql(db, "DROP TRIGGER rollback_all");
    assert(!commit(db, "1", path) && !commit(db, "1", path));
    assert(!media_index_commit(db, &result, (const uint8_t[32]){1}, NULL, "1", path, root));
    assert(scalar(db, "SELECT count(*) FROM media_files WHERE node_sign!=zeroblob(64)") == 2);
    assert(media_index_register_downloaded(db, result.media_id, ids, result.content_hash, "conflict", "wrong", (const uint8_t[32]){1}, 14, 7, 0, 0) == -1);
    assert(scalar(db, "SELECT count(*) FROM media_files WHERE chat_id='1'") == 2);
    sql(db, "DELETE FROM media_files");
    char long_path[MEDIA_PATH_MAX], relative[301]; memset(relative, 'x', sizeof(relative) - 1); relative[300] = 0;
    assert(!media_index_path(root, relative, long_path));
    int previous_limit = sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 200);
    assert(commit(db, "1", long_path) == -1 && !scalar(db, "SELECT count(*) FROM media_files"));
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, previous_limit);
    result.num_blocks = INT_MAX;
    assert(commit(db, "1", path) == -1); result.num_blocks = 2;
    sqlite3_close(db);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "media regression schema/atomicity/savepoint/duplicates PASS");
}

static void deletion(void) {
    sqlite3* db = database(); char path[MEDIA_PATH_MAX]; assert(!media_index_path(root, "delete", path));
    write_file(path); assert(!commit(db, "1", path));
    sql(db, "BEGIN"); assert(media_index_delete_channel(db, "1", root) == -1 && ma_file_size(path) == 14); sql(db, "ROLLBACK");
    sql(db, "CREATE TRIGGER fail_delete BEFORE DELETE ON media_files BEGIN SELECT RAISE(ABORT,'delete failure'); END");
    assert(media_index_delete_channel(db, "1", root) == -1 && ma_file_size(path) == -1);
    assert(scalar(db, "SELECT count(*) FROM media_files") == 2);
    sql(db, "DROP TRIGGER fail_delete");
    assert(!media_index_delete_channel(db, "1", root) && !scalar(db, "SELECT count(*) FROM media_files"));
    assert(!media_index_delete_channel(db, "1", root));
    write_file(path); assert(!commit(db, "1", path));
    uint8_t other[16] = {44};
    assert(!media_index_register_downloaded(db, result.media_id, other, result.content_hash, "2", "delete", (const uint8_t[32]){1}, 14, 7, 0, 0));
    assert(!media_index_delete_channel(db, "1", root) && ma_file_size(path) == 14);
    assert(!media_index_delete_channel(db, "2", root) && ma_file_size(path) == -1);
    assert(!utun_mkdir(path, 0700)); assert(!commit(db, "1", path));
    assert(media_index_delete_channel(db, "1", root) == -1 && scalar(db, "SELECT count(*) FROM media_files") == 2);
    assert(!test_rmdir(path)); assert(!media_index_delete_channel(db, "1", root));
    /* Подготовка могла создать файл и упасть до первого INSERT блока. */
    write_file(path); assert(!media_index_track_preparation(db, "1", path, root));
    assert(!media_index_delete_channel(db, "1", root) && ma_file_size(path) == -1);
    assert(!scalar(db, "SELECT count(*) FROM media_preparations"));
    sql(db, "DROP TABLE media_files"); assert(media_index_delete_channel(db, "1", root) == -1);
    sqlite3_close(db);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "media regression deletion retry/shared/unprepared files PASS");
}

static void long_path(void) {
#ifndef _WIN32
    char path[MEDIA_PATH_MAX]; snprintf(path, sizeof(path), "%s", root);
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(path); path[n++] = '/'; memset(path + n, 'a' + i, 180); path[n + 180] = 0;
        assert(!utun_mkdir(path, 0700));
    }
    strcat(path, "/payload"); write_file(path); sqlite3* db = database();
    assert(!commit(db, "1", path));
    sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(db, "SELECT location FROM media_files LIMIT 1", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW); const char* location = (const char*)sqlite3_column_text(st, 0);
    char restored[MEDIA_PATH_MAX];
    assert(strlen(location) == 550 && !media_index_path(root, location, restored) && !strcmp(restored, path));
    assert(ma_file_size(restored) == 14); sqlite3_finalize(st);
    assert(!media_index_delete_channel(db, "1", root) && ma_file_size(path) == -1); sqlite3_close(db);
#endif
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "media regression long location PASS");
}

static int deleted_events, failed_submissions, successful_submissions;
static char delete_from_event[65];
static void download_done(void* arg, int error) { assert(error); (*(int*)arg)++; }
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int length) {
    if (type == CHAT_EVT_CHANNEL_DELETED) deleted_events++;
    if (type == CHAT_EVT_MSG_SUBMITTED) {
        assert(length == 9);
        if (data[8]) successful_submissions++; else failed_submissions++;
        if (delete_from_event[0]) {
            int before = deleted_events;
            assert(CC(inst)->media_submissions); chat_core_delete_channel(inst, delete_from_event);
            assert(deleted_events == before); delete_from_event[0] = 0;
        }
    }
}

static void pending_deletion(void) {
    char config[1024]; snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", root);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst);
    assert(!utun_core_start(inst));
    struct denied denied = {.read_schema = 1};
    assert(!sqlite3_set_authorizer(inst->topo_sqlite_db, authorizer, &denied));
    assert(chat_service_start(inst) == -1 && !inst->chat_started && !inst->chat_core);
    assert(!sqlite3_set_authorizer(inst->topo_sqlite_db, NULL, NULL));
    assert(!chat_service_start(inst)); chat_event_set_handler(inst, event);
    chat_core_create_channel_auto(inst, "pending");
    sqlite3_stmt* st = NULL; sqlite3* db = inst->topo_sqlite_db;
    assert(!sqlite3_prepare_v2(db, "SELECT channel_id,owner_node_id,x25519_pubkey,x25519_privkey,ed25519_pubkey,"
        "ed25519_privkey,signature FROM channels WHERE name='pending'", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW); char channel[65]; snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0));
    struct chat_channel_create recreate = {.inst = inst};
    snprintf(recreate.channel_id, sizeof(recreate.channel_id), "%s", channel);
    snprintf(recreate.name, sizeof(recreate.name), "pending"); assert(sqlite3_column_bytes(st, 1) == 32); memcpy(recreate.owner_node_id, sqlite3_column_blob(st, 1), 32);
    uint8_t* keys[] = {recreate.x25519_pubkey, recreate.x25519_privkey, recreate.ed25519_pubkey, recreate.ed25519_privkey, recreate.signature};
    for (int i = 0; i < 5; i++) {
        int size = i == 4 ? 64 : 32; assert(sqlite3_column_bytes(st, i + 2) == size);
        memcpy(keys[i], sqlite3_column_blob(st, i + 2), size);
    }
    sqlite3_finalize(st);
    char source[MEDIA_PATH_MAX], destinations[2][MEDIA_PATH_MAX];
    assert(!media_index_path(root, "source", source)); write_file(source);
    struct chat_msg_submit req = {.inst = inst, .media_copy = 1, .data = (uint8_t*)"payload", .data_len = 7};
    snprintf(req.channel_id, sizeof(req.channel_id), "%s", channel);
    for (int i = 0; i < 2; i++) {
        char relative[32]; snprintf(relative, sizeof(relative), "outgoing%d", i);
        assert(!media_index_path(root, relative, destinations[i]));
        snprintf(req.media_src, sizeof(req.media_src), "%s", source);
        snprintf(req.media_dest, sizeof(req.media_dest), "%s", destinations[i]);
        req.request_id = i + 1; chat_core_submit_media_message(inst, &req, NULL, NULL);
    }
    assert(CC(inst)->media_submissions && !failed_submissions);
    assert(scalar(db, "SELECT count(*) FROM media_preparations") == 2); /* completions не обработаны */
    chat_core_delete_channel(inst, channel); chat_core_delete_channel(inst, channel);
    assert(!deleted_events && scalar(db, "SELECT count(*) FROM channels WHERE name='pending'") == 1);
    req.request_id = 3; chat_core_submit_media_message(inst, &req, NULL, NULL); assert(failed_submissions == 1);
    for (int i = 0; i < 2000 && !deleted_events; i++) uasync_poll(ua, 100);
    assert(deleted_events == 1 && failed_submissions == 3 && !successful_submissions && !CC(inst)->media_submissions);
    assert(!scalar(db, "SELECT count(*) FROM channels WHERE name='pending'"));
    assert(!scalar(db, "SELECT count(*) FROM media_files") && !scalar(db, "SELECT count(*) FROM media_preparations"));
    assert(ma_file_size(source) == 14 && ma_file_size(destinations[0]) == -1 && ma_file_size(destinations[1]) == -1);
    chat_core_create_channel(inst, &recreate); uasync_poll(ua, 0);
    assert(scalar(db, "SELECT count(*) FROM channels WHERE name='pending'") == 1 && si_find(inst, channel));
    assert(!scalar(db, "SELECT count(*) FROM media_files"));

    /* Обработчик MSG_SUBMITTED синхронный: удаление должно увидеть ещё живую задачу. */
    chat_core_create_channel_auto(inst, "reentrant");
    assert(!sqlite3_prepare_v2(db, "SELECT channel_id FROM channels WHERE name='reentrant'", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW);
    snprintf(req.channel_id, sizeof(req.channel_id), "%s", sqlite3_column_text(st, 0)); sqlite3_finalize(st);
    snprintf(delete_from_event, sizeof(delete_from_event), "%s", req.channel_id);
    req.request_id = 4; chat_core_submit_media_message(inst, &req, NULL, NULL);
    for (int i = 0; i < 2000 && deleted_events < 2; i++) uasync_poll(ua, 100);
    assert(deleted_events == 2 && successful_submissions == 1 && failed_submissions == 3 && !CC(inst)->media_submissions);
    assert(!scalar(db, "SELECT count(*) FROM channels WHERE name='reentrant'"));
    assert(ma_file_size(destinations[1]) == -1 && ma_file_size(source) == 14);

    /* Отказ/синхронное завершение/дубликат имеют разные правила владения аргументом. */
    uint8_t signatures[128] = {0}; struct media_index_result download = result; download.block_sigs = signatures;
    int completed = 0;
    uint64_t group = topo_groups_channel_slot(inst->topo_groups, channel);
    assert(test_member_seed(inst, channel, inst->my_keys.public_key, inst->my_ed25519_pubkey,
        inst->my_ed25519_privkey, recreate.ed25519_privkey, "{}", NULL, NULL) >= 0);
    assert(media_download_start(inst, 0, &download, destinations[0], root, inst->my_ed25519_pubkey,
        download_done, &completed, NULL, NULL) == -1 && !completed); // Несуществующая группа.
    assert(media_download_start(inst, group, &download, destinations[0], root, 0, download_done, &completed, NULL, NULL) == -1);
    assert(!completed); /* Отсутствующий ключ автора — отказ до принятия владения callback. */
    media_test_publish_channel(db, channel, inst->node_id, &download);
    assert(!media_download_start(inst, group, &download, destinations[0], root, inst->my_ed25519_pubkey,
                                 download_done, &completed, NULL, NULL));
    assert(completed == 1); /* Нет суперузлов: completion уже вызван, dl освобождён. */
    assert(!media_index_delete_channel(db, channel, root));
    completed = 0;
    struct SC_MYKEYS author_keys; uint8_t author_ed[32], author_private[64];
    assert(!sc_identity_generate(author_keys.public_key, author_keys.private_key, author_ed, author_private));
    uint64_t author = sc_derive_node_id_from_pubkey(author_keys.public_key);
    assert(test_member_seed(inst, channel, author_keys.public_key, author_ed, author_private,
        recreate.ed25519_privkey, "{}", NULL, NULL) >= 0);
    download.media_id[0]++;
    media_test_publish_channel(db, channel, author, &download);
    assert(!media_download_start(inst, group, &download, destinations[0], root, author_ed,
                                 download_done, &completed, NULL, NULL));
    assert(!completed);
    assert(!media_download_start(inst, group, &download, destinations[0], root, author_ed,
                                download_done, &completed, NULL, NULL));
    assert(!completed && !media_download_cancel(inst, download.media_id, NULL, 0) && completed == 2);
    assert(!media_index_delete_channel(db, channel, root));
    utun_instance_destroy(inst); uasync_destroy(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "media regression pending deletion/two completions/recreation PASS");
}

#ifdef __linux__
static void preparation_failed(void* arg, int error, const struct media_index_result* value) {
    int* completed = arg; assert(error && !value); (*completed)++;
}
static void failed_preparation(void) {
    char source[MEDIA_PATH_MAX], destination[MEDIA_PATH_MAX];
    assert(!media_index_path(root, "copy-source", source) && !media_index_path(root, "hash-failure", destination));
    write_file(source); sqlite3* db = database();
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct media_async* ma = media_async_create(); assert(ma); int completed = 0;
    media_index_register_async(ma, ua, db, (const uint8_t[32]){1}, key, "failed", source, destination, 1, root, preparation_failed, &completed);
    for (int i = 0; i < 2000 && !completed; i++) uasync_poll(ua, 100);
    assert(completed == 1 && ma_file_size(destination) == 14 && !scalar(db, "SELECT count(*) FROM media_files"));
    assert(scalar(db, "SELECT count(*) FROM media_preparations") == 1);
    assert(!media_index_delete_channel(db, "failed", root));
    assert(ma_file_size(destination) == -1 && ma_file_size(source) == 14);
    media_async_destroy(ma); uasync_destroy(ua, 0); sqlite3_close(db);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "media regression failed preparation cleanup PASS");
}
#endif

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    assert(!test_mkdtemp(root)); ids[16] = 2;
    paths(); transactions(); deletion(); long_path(); pending_deletion();
#ifdef __linux__
    failed_preparation();
#endif
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "test_media_index_regressions PASS"); return 0;
}
