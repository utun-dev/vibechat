/* Долговечные задания и ссылки общих вложений: rollback публикации, повторы и изоляция владельцев. */
#include "../src/attachments/store.h"
#include "../src/attachments/observe.h"
#include "../src/attachments/manifest.h"
#include "../lib/mem.h"
#include "../src/db_schema.h"
#include "../lib/debug_config.h"
#include "test_utils.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sql(sqlite3* db, const char* query) {
    char* error = NULL; int rc = sqlite3_exec(db, query, NULL, NULL, &error);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "store test SQL rc=%d query=%s error=%s", rc, query, error ? error : "");
    sqlite3_free(error); assert(rc == SQLITE_OK);
}
static int count(sqlite3* db, const char* query) {
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, query, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int result = sqlite3_column_int(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return result;
}
static int veto(void* data) { (*(int*)data)++; return 1; }

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    char directory[768], file[1024]; const char* temporary = getenv("TMPDIR");
    snprintf(directory, sizeof(directory), "%s/attachment-store-XXXXXX", temporary ? temporary : "/tmp");
    assert(!test_mkdtemp(directory)); snprintf(file, sizeof(file), "%s/store.db", directory);
    sqlite3* db = NULL; assert(sqlite3_open(file, &db) == SQLITE_OK); sql(db, "PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL");
    assert(!db_schema_init(db));
    assert(!db_schema_check(db));
    #define TEST_MEDIA_GROUP "bb5c672482b0dcca91a21a4ed63b15afde8aa1378da72cd01b349589d6e7dd6a"
    static const uint8_t request[] = {1, 0, 2, 0, 3}, manifest[] = {4, 0, 5, 6};
    struct attachment_job job = {.id={1}, .namespace_token={2}, .owner=ATTACHMENT_OWNER_CHAT, .space=TEST_MEDIA_GROUP, .author={9},
        .source="/source/user-file", .request=request, .request_size=sizeof(request)};
    struct attachment_object object = {.scope=ATTACHMENT_SCOPE_GROUP, .space=TEST_MEDIA_GROUP, .author={9}, .size=123, .id={3}, .hash={4},
        .crypto=ATTACHMENT_CRYPTO_NONE, .manifest=manifest, .manifest_size=sizeof(manifest)};
    struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_CHAT, .space=TEST_MEDIA_GROUP, .message={5}, .object={3}};
    enum attachment_job_state state = 0; uint8_t message[32];
    assert(attachment_store_begin_write(db, &job) == SQLITE_MISUSE);
    sql(db, "BEGIN IMMEDIATE"); assert(!attachment_store_prepare_write(db)); assert(!attachment_store_begin_write(db, &job));
    assert(!attachment_store_begin_write(db, &job)); sql(db, "COMMIT");
    assert(!attachment_store_job_read(db, job.id, &state, message) && state == ATTACHMENT_JOB_PREPARING && !message[0]);
    sql(db, "BEGIN IMMEDIATE"); job.source="/different/source";
    assert(attachment_store_begin_write(db, &job) == SQLITE_CONSTRAINT); sql(db, "ROLLBACK"); job.source="/source/user-file";
    sql(db, "BEGIN IMMEDIATE"); assert(attachment_store_publish_write(db, job.id, &ref) == SQLITE_CONSTRAINT); sql(db, "ROLLBACK");
    sql(db, "BEGIN IMMEDIATE"); assert(!attachment_store_ready_write(db, job.id, &object, "/cache/immutable-file")); sql(db, "COMMIT");
    assert(sqlite3_close(db) == SQLITE_OK); db = NULL;
    assert(sqlite3_open(file, &db) == SQLITE_OK); sql(db, "PRAGMA foreign_keys=ON");
    assert(!attachment_store_job_read(db, job.id, &state, message) && state == ATTACHMENT_JOB_READY && !message[0]);
    assert(!attachment_store_public_read(db, object.space, object.id));
    assert(!attachment_store_authorize_read(db, job.id, job.owner, job.space, job.author, job.namespace_token));
    assert(attachment_store_authorize_read(db, job.id, ATTACHMENT_OWNER_KANBAN, job.space, job.author, job.namespace_token) == SQLITE_AUTH);
    assert(attachment_store_authorize_read(db, job.id, job.owner, job.space, (const uint8_t[32]){9, [31]=1}, job.namespace_token) == SQLITE_AUTH);
    uint8_t revoked[32] = {99};
    assert(attachment_store_authorize_read(db, job.id, job.owner, job.space, job.author, revoked) == SQLITE_AUTH);
    sql(db, "BEGIN IMMEDIATE"); assert(!attachment_store_ready_write(db, job.id, &object, "/cache/immutable-file")); sql(db, "COMMIT");
    int rejected = 0; sqlite3_commit_hook(db, veto, &rejected);
    sql(db, "BEGIN IMMEDIATE;INSERT INTO ui_state(key,value) VALUES('attachment-message','signed message')");
    assert(!attachment_store_publish_write(db, job.id, &ref));
    assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_CONSTRAINT && rejected == 1 && sqlite3_get_autocommit(db));
    sqlite3_commit_hook(db, NULL, NULL);
    assert(!count(db, "SELECT count(*) FROM ui_state WHERE key='attachment-message'"));
    assert(!count(db, "SELECT count(*) FROM attachment_refs"));
    assert(!attachment_store_public_read(db, object.space, object.id));
    assert(!attachment_store_job_read(db, job.id, &state, message) && state == ATTACHMENT_JOB_READY);
    sql(db, "BEGIN IMMEDIATE;INSERT INTO ui_state(key,value) VALUES('attachment-message','signed message')");
    assert(!attachment_store_publish_write(db, job.id, &ref)); sql(db, "COMMIT");
    sql(db, "BEGIN IMMEDIATE"); assert(!attachment_store_publish_write(db, job.id, &ref)); sql(db, "COMMIT");
    assert(!attachment_store_job_read(db, job.id, &state, message) && state == ATTACHMENT_JOB_PUBLISHED && !memcmp(message, ref.message, 32));
    assert(count(db, "SELECT count(*) FROM attachment_refs") == 1);
    assert(attachment_store_public_read(db, object.space, object.id) == 1);
    sql(db, "BEGIN IMMEDIATE"); ref.message[0]=6;
    assert(attachment_store_publish_write(db, job.id, &ref) == SQLITE_CONSTRAINT); sql(db, "ROLLBACK"); ref.message[0]=5;
    sql(db, "BEGIN IMMEDIATE"); object.hash[0]=8;
    assert(attachment_store_observe_write(db, &object, &ref, NULL) == SQLITE_CONSTRAINT);
    int conflict = 0; ref.message[0] = 6;
    assert(!attachment_store_observe_write(db, &object, &ref, &conflict) && conflict);
    sql(db, "COMMIT");
    assert(count(db, "SELECT count(*) FROM attachment_refs") == 1);
    object.hash[0]=4; ref.message[0] = 5;
    sql(db, "BEGIN IMMEDIATE"); ref.owner=ATTACHMENT_OWNER_KANBAN;
    assert(!attachment_store_observe_write(db, &object, &ref, NULL)); sql(db, "COMMIT");
    assert(count(db, "SELECT count(*) FROM attachment_objects") == 1 && count(db, "SELECT count(*) FROM attachment_refs") == 2);
    sql(db, "BEGIN IMMEDIATE"); ref.owner=ATTACHMENT_OWNER_CHAT;
    assert(!attachment_store_release_write(db, &ref)); assert(!attachment_store_observe_write(db, &object, &ref, NULL)); sql(db, "COMMIT");
    assert(count(db, "SELECT count(*) FROM attachment_refs WHERE released=0") == 1);
    assert(count(db, "SELECT count(*) FROM attachment_objects") == 1);
    assert(attachment_store_public_read(db, object.space, object.id) == 1);
    sql(db, "BEGIN IMMEDIATE"); ref.owner=ATTACHMENT_OWNER_DM;
    assert(attachment_store_observe_write(db, &object, &ref, NULL) == SQLITE_MISUSE); sql(db, "ROLLBACK");
    object.scope=ATTACHMENT_SCOPE_DM; object.crypto=ATTACHMENT_CRYPTO_DM;
    strcpy(ref.space, "7"); strcpy(object.space, ref.space);
    sql(db, "BEGIN IMMEDIATE"); assert(!attachment_store_observe_write(db, &object, &ref, NULL)); sql(db, "COMMIT");
    assert(count(db, "SELECT count(*) FROM attachment_objects") == 2 && count(db, "SELECT count(*) FROM attachment_refs") == 3);
    /* MKV наблюдается как видеовложение и попадает в каталог для штатной загрузки. */
    uint8_t block_id[16] = {21}, block_signature[64] = {22}, mkv_message[32] = {23};
    struct media_index_result mkv = {.file_size=123, .block_size=1024, .num_blocks=1,
        .media_id={19}, .content_hash={20}, .block_ids=block_id, .block_sigs=block_signature};
    char* mkv_body = attachment_manifest_format("recording.mkv", 13, &mkv, 1234, 1920, 1080); assert(mkv_body);
    char mkv_json[2048]; snprintf(mkv_json, sizeof(mkv_json), "{\"ct\":\"video/x-matroska\",\"d\":\"%s\"}", mkv_body); u_free(mkv_body);
    sql(db, "BEGIN IMMEDIATE");
    assert(!attachment_manifest_observe_write(db, ATTACHMENT_OWNER_CHAT, TEST_MEDIA_GROUP, mkv_message, job.author, mkv_json, strlen(mkv_json), NULL));
    sql(db, "COMMIT"); assert(attachment_store_public_read(db, TEST_MEDIA_GROUP, mkv.media_id) == 1);
    assert(count(db, "SELECT count(*) FROM attachment_objects") == 3);
    assert(sqlite3_close(db) == SQLITE_OK);
    puts("PASS attachment store: durable intent/READY, atomic publication rollback, idempotency, immutable objects, owner and access scopes");
    return 0;
}
