// media_index.c — таблица media_files: init, commit, register_async
#include "media_index.h"
#include "../db_schema.h"
#include "../media_async/media_async.h"
#include "../../lib/u_async.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/file_io.h"
#include "../../lib/platform_compat.h"

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <errno.h>

#define MEDIA_BLOCK_MIN   (10 * 1024 * 1024)
#define MEDIA_BLOCK_MAX   (25 * 1024 * 1024)
#define MEDIA_BLOCK_TARGET 30

/* Нормализация только синтаксиса: ссылки каталогов здесь не разрешаются. */
static int mi_clean_path(const char* input, char* output, size_t capacity) {
    if (!input || !*input || strlen(input) >= capacity) goto invalid;
    size_t used = 0, pos = 0;
    char source[MEDIA_PATH_MAX];
    if (capacity > sizeof(source)) goto invalid;
    memcpy(source, input, strlen(input) + 1);
#ifdef _WIN32
    for (size_t i = 0; source[i]; i++) if (source[i] == '\\') source[i] = '/';
    if (source[0] && source[1] == ':') {
        if (source[2] != '/' || !((source[0] >= 'A' && source[0] <= 'Z') || (source[0] >= 'a' && source[0] <= 'z')))
            goto invalid;
        output[used++] = source[0]; output[used++] = ':'; output[used++] = '/'; pos = 3;
    } else if (source[0] == '/' && source[1] == '/') {
        output[used++] = '/'; output[used++] = '/'; pos = 2;
    } else
#endif
    if (source[0] == '/') { output[used++] = '/'; pos = 1; }
    while (source[pos]) {
        if (source[pos] == '/') { pos++; continue; }
        size_t start = pos;
        while (source[pos] && source[pos] != '/') pos++;
        size_t length = pos - start;
        if (length == 1 && source[start] == '.') continue;
        if (length == 2 && source[start] == '.' && source[start + 1] == '.') goto invalid;
        for (size_t i = start; i < pos; i++) if (source[i] == ':') goto invalid;
        if (used && output[used - 1] != '/') output[used++] = '/';
        if (used + length >= capacity) goto invalid;
        memcpy(output + used, source + start, length); used += length;
    }
    if (!used) output[used++] = '.';
    output[used] = 0;
    return 0;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index invalid/oversized path=%s limit=%zu", input ? input : "(null)", capacity);
    return -1;
}

/* location намеренно строго канонический: одинаковое имя должно иметь одно представление в БД. */
static int mi_location_valid(const char* location) {
    char clean[MEDIA_PATH_MAX];
    if (location && *location && location[0] != '/' && !strchr(location, '\\') && !strchr(location, ':')
        && !mi_clean_path(location, clean, sizeof(clean)) && strcmp(clean, ".") && !strcmp(location, clean)) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index invalid location=%s", location ? location : "(null)"); return -1;
}

int media_index_path(const char* base, const char* location, char path[MEDIA_PATH_MAX]) {
    char clean[MEDIA_BASE_MAX], full[MEDIA_PATH_MAX];
    if (!path) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index path without output"); return -1; }
    if (mi_clean_path(base, clean, sizeof(clean)) || mi_location_valid(location)) return -1;
    int size = !strcmp(clean, ".") ? snprintf(full, sizeof(full), "%s", location)
        : snprintf(full, sizeof(full), "%s%s%s", clean, clean[strlen(clean) - 1] == '/' ? "" : "/", location);
    if (size >= MEDIA_PATH_MAX || size < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index joined path too long base=%s location=%s", clean, location); return -1;
    }
    memcpy(path, full, (size_t)size + 1);
    return 0;
}

int media_index_location(const char* path, const char* base, char location[MEDIA_PATH_MAX]) {
    char full[MEDIA_PATH_MAX], root[MEDIA_BASE_MAX];
    if (!location) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index location without output"); return -1; }
    if (mi_clean_path(path, full, sizeof(full)) || mi_clean_path(base, root, sizeof(root))) return -1;
    size_t length = strlen(root);
    const char* relative = NULL;
    if (!strcmp(root, ".") && full[0] != '/' && !strchr(full, ':')) relative = full;
    else if (!strncmp(full, root, length)) {
        if (root[length - 1] == '/') relative = full + length;
        else if (full[length] == '/') relative = full + length + 1;
    }
    if (!relative || mi_location_valid(relative)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index path outside base path=%s base=%s", full, root); return -1;
    }
    memcpy(location, relative, strlen(relative) + 1);
    return 0;
}

/* Единая диагностика SQL, включая ошибки управления savepoint. */
static int mi_exec(sqlite3* db, const char* sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index SQL failed rc=%d sql=%s: %s", rc, sql, sqlite3_errmsg(db)); return -1;
}

int media_index_base_read(sqlite3* db, char base[MEDIA_BASE_MAX]) {
    if (base) base[0] = 0;
    if (!db || !base) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index base read requires SQL owner/output"); return SQLITE_MISUSE; }
    sqlite3_stmt* st = NULL; const char* stage = "prepare"; char clean[MEDIA_BASE_MAX];
    int rc = sqlite3_prepare_v2(db, "SELECT value FROM ui_state WHERE key='media_base'", -1, &st, NULL); if (rc) goto done;
    stage = "step"; rc = sqlite3_step(st); if (rc == SQLITE_DONE) { stage = "missing media_base"; rc = SQLITE_NOTFOUND; goto done; }
    if (rc != SQLITE_ROW) goto done;
    stage = "stored type/size"; rc = SQLITE_MISMATCH;
    if (sqlite3_column_type(st, 0) != SQLITE_TEXT) goto done;
    const char* value = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0);
    if (!value) { stage = "text allocation"; rc = SQLITE_NOMEM; goto done; }
    if (bytes <= 0 || memchr(value, 0, (size_t)bytes)) goto done;
    if (bytes >= MEDIA_BASE_MAX) { rc = SQLITE_TOOBIG; goto done; }
    stage = "stored path syntax"; if (mi_clean_path(value, clean, sizeof(clean))) goto done;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (!rc) { memcpy(base, clean, strlen(clean) + 1); return SQLITE_OK; }
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index base read cleanup finalize rc=%d", final); }
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index base read stage=%s rc=%d: %s", stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc)); return rc;
}
int media_index_get_base(sqlite3* db, char base[MEDIA_BASE_MAX]) { return media_index_base_read(db, base) ? -1 : 0; }

int media_index_base_set(sqlite3* db, const char* base) {
    char clean[MEDIA_BASE_MAX]; sqlite3_stmt* st = NULL;
    if (!db || !base || !*base) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index set base requires SQL owner/path"); return SQLITE_MISUSE; }
    if (strlen(base) >= sizeof(clean)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index set base path too long"); return SQLITE_TOOBIG; }
    if (mi_clean_path(base, clean, sizeof(clean))) return SQLITE_MISMATCH;
    int rc = db_schema_table_ex(db, "ui_state", NULL); if (rc) return rc;
    const char* stage = "prepare";
    rc = sqlite3_prepare_v2(db, "INSERT INTO ui_state(key,value) VALUES('media_base',?) ON CONFLICT(key) DO NOTHING", -1, &st, NULL);
    stage = rc ? stage : "bind";
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, clean, -1, SQLITE_STATIC);
    stage = rc ? stage : "step";
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int finish = sqlite3_finalize(st); if (rc == SQLITE_DONE) { rc = finish; stage = "finalize"; }
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index set base requested=%s stage=%s rc=%d finalize=%d: %s", clean, stage, rc, finish,
            (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc)); return rc;
    }
    char stored[MEDIA_BASE_MAX];
    rc = media_index_base_read(db, stored); if (rc) return rc;
    if (strcmp(stored, clean)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index base change rejected requested=%s stored=%s", clean, stored); return SQLITE_CONSTRAINT;
    }
    return SQLITE_OK;
}
int media_index_set_base(sqlite3* db, const char* base) { return media_index_base_set(db, base) ? -1 : 0; }

int media_index_track_preparation_write(sqlite3* db, const char* chat_id, const char* path, const char* base) {
    char location[MEDIA_PATH_MAX]; sqlite3_stmt* st = NULL;
    if (!db || !chat_id || !*chat_id || strlen(chat_id) > 64 || media_index_location(path, base, location)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index invalid preparation path/channel"); return SQLITE_MISUSE;
    }
    int rc = sqlite3_prepare_v2(db, "INSERT INTO media_preparations(chat_id,location) VALUES(?,?) ON CONFLICT DO NOTHING",
                              -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, chat_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, location, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int finish = st ? sqlite3_finalize(st) : SQLITE_OK;
    if (rc == SQLITE_DONE && finish == SQLITE_OK) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media_index preparation tracked chat=%s path=%s", chat_id, location); return 0;
    }
    if (rc == SQLITE_DONE) rc = finish;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index track preparation chat=%s rc=%d: %s", chat_id, rc, sqlite3_errstr(rc)); return rc;
}
int media_index_track_preparation(sqlite3* db, const char* chat_id, const char* path, const char* base) {
    return media_index_track_preparation_write(db, chat_id, path, base) ? -1 : 0;
}

static int mi_insert_write(sqlite3* db,
                     const uint8_t* media_id, const uint8_t* block_id,
                     const uint8_t* content_hash,
                     const char* chat_id, const char* location,
                     const uint8_t holder[32], const uint8_t* node_sign,
                     int64_t timestamp, int64_t file_size,
                     int64_t chunk_size, int chunk, int64_t offset) {
    const char* sql =
        "INSERT INTO media_files(media_id,block_id,content_hash,chat_id,location,"
        "holder,node_sign,timestamp,file_size,chunk_size,chunk,offset)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) ON CONFLICT(block_id) DO UPDATE SET block_id=excluded.block_id"
        " WHERE media_files.media_id=excluded.media_id AND media_files.content_hash=excluded.content_hash"
        " AND media_files.chat_id=excluded.chat_id AND media_files.location=excluded.location"
        " AND media_files.holder=excluded.holder AND media_files.file_size=excluded.file_size"
        " AND media_files.chunk_size=excluded.chunk_size AND media_files.chunk=excluded.chunk AND media_files.offset=excluded.offset";

    sqlite3_stmt* stmt = NULL;
    int prepared = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (prepared != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index: insert prep fail: %s", sqlite3_errmsg(db));
        return prepared;
    }
    int rc = SQLITE_OK;
#define MI_BIND(expression) do { if (rc == SQLITE_OK) rc = (expression); } while (0)
    MI_BIND(sqlite3_bind_blob(stmt, 1, media_id, 16, SQLITE_STATIC));
    MI_BIND(sqlite3_bind_blob(stmt, 2, block_id, 16, SQLITE_STATIC));
    MI_BIND(sqlite3_bind_blob(stmt, 3, content_hash, 32, SQLITE_STATIC));
    MI_BIND(sqlite3_bind_text(stmt, 4, chat_id, -1, SQLITE_STATIC));
    MI_BIND(sqlite3_bind_text(stmt, 5, location, -1, SQLITE_STATIC));
    MI_BIND(sqlite3_bind_blob(stmt, 6, holder, 32, SQLITE_STATIC));
    MI_BIND(sqlite3_bind_blob(stmt, 7, node_sign, 64, SQLITE_STATIC));
    MI_BIND(sqlite3_bind_int64(stmt, 8, timestamp));
    MI_BIND(sqlite3_bind_int64(stmt, 9, file_size));
    MI_BIND(sqlite3_bind_int64(stmt, 10, chunk_size));
    MI_BIND(sqlite3_bind_int(stmt, 11, chunk));
    MI_BIND(sqlite3_bind_int64(stmt, 12, offset));
#undef MI_BIND
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(db), finish = sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE || finish != SQLITE_OK || changed != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index %s chat=%s block=%02x%02x%02x%02x chunk=%d path=%s rc=%d: %s",
                    rc == SQLITE_DONE && finish == SQLITE_OK && !changed ? "conflicting block metadata" : "insert failed",
                    chat_id, block_id[0], block_id[1], block_id[2], block_id[3], chunk, location, rc, sqlite3_errmsg(db));
        return rc != SQLITE_DONE ? rc : finish != SQLITE_OK ? finish : SQLITE_CONSTRAINT;
    }
    return 0;
}

int media_index_init(sqlite3* db) {
    if (!db) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index init without DB"); return -1; }

    if (db_schema_table(db, "media_files", NULL) || db_schema_table(db, "media_preparations", NULL) || db_schema_attachment_views(db)) return -1;

    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media_index: table+indices created");
    return 0;
}

int media_calc_block_size(uint64_t file_size) {
    if (file_size == 0) return 0;
    uint64_t target = (file_size - 1) / MEDIA_BLOCK_TARGET + 1;
    if (target < MEDIA_BLOCK_MIN) target = MEDIA_BLOCK_MIN;
    if (target > MEDIA_BLOCK_MAX) target = MEDIA_BLOCK_MAX;
    return (int)target;
}

/* Удаление regular/link без перехода через конечную ссылку; каталоги не удаляются. */
static int mi_remove(const char* path) {
    struct file_io_info info;
    int found = file_io_inspect(path, 0, &info);
    if (found == 1) return 0;
    if (found || (info.type != FILE_IO_REGULAR && info.type != FILE_IO_LINK)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index cannot remove non-file path=%s", path); return -1;
    }
#ifdef _WIN32
    wchar_t wide[FILE_IO_PATH_MAX];
    if (file_io_wide(path, wide)) return -1;
    int rc = _wremove(wide);
#else
    int rc = remove(path);
#endif
    if (!rc || errno == ENOENT) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index remove failed path=%s errno=%d: %s", path, errno, strerror(errno));
    return -1;
}

int media_index_delete_channel(sqlite3* db, const char* chat_id, const char* media_base) {
    char checked_base[MEDIA_BASE_MAX];
    if (!db || !chat_id || !*chat_id || !sqlite3_get_autocommit(db) || mi_clean_path(media_base, checked_base, sizeof(checked_base))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index delete invalid args/external transaction"); return -1;
    }
    struct mi_delete_file { char location[MEDIA_PATH_MAX]; char path[MEDIA_PATH_MAX]; int shared; };
    struct mi_delete_file* files = NULL; uint32_t count = 0;
    sqlite3_stmt* st = NULL;
    const char* query = "WITH paths AS (SELECT chat_id,location FROM media_files UNION SELECT chat_id,location FROM media_preparations)"
        " SELECT location,EXISTS(SELECT 1 FROM paths other WHERE other.location=mine.location AND other.chat_id!=mine.chat_id)"
        " FROM paths mine WHERE chat_id=?";
    int rc = sqlite3_prepare_v2(db, query, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, chat_id, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto failed;
    /* Полный снимок и проверка всех путей до первого необратимого удаления. */
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* location = (const char*)sqlite3_column_text(st, 0);
        char path[MEDIA_PATH_MAX];
        if (!location || sqlite3_column_bytes(st, 0) != (int)strlen(location)
            || media_index_path(media_base, location, path) || count >= UINT32_MAX / sizeof(*files) - 1) goto failed;
        struct mi_delete_file* grown = u_realloc(files, (count + 1) * sizeof(*files));
        if (!grown) goto failed;
        files = grown;
        snprintf(files[count].location, sizeof(files[count].location), "%s", location);
        memcpy(files[count].path, path, strlen(path) + 1);
        files[count].shared = sqlite3_column_int(st, 1); count++;
    }
    if (rc != SQLITE_DONE) goto failed;
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto failed;
    rc = sqlite3_prepare_v2(db, "DELETE FROM media_preparations WHERE chat_id=? AND location=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto failed;
    int errors = 0, rows = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!files[i].shared && mi_remove(files[i].path)) { errors++; continue; }
        sqlite3_stmt* blocks = NULL;
        rc = sqlite3_prepare_v2(db, "DELETE FROM media_files WHERE chat_id=? AND location=?", -1, &blocks, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(blocks, 1, chat_id, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(blocks, 2, files[i].location, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(blocks);
        int finish = blocks ? sqlite3_finalize(blocks) : SQLITE_OK;
        if (rc != SQLITE_DONE || finish != SQLITE_OK) goto failed;
        rows += sqlite3_changes(db);
        rc = sqlite3_bind_text(st, 1, chat_id, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, files[i].location, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto failed;
        rc = sqlite3_reset(st);
        if (rc != SQLITE_OK) goto failed;
    }
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto failed;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media_index delete chat=%s paths=%u rows=%d failures=%d", chat_id, count, rows, errors);
    u_free(files); return errors ? -1 : 0;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index incomplete delete chat=%s rc=%d paths=%u: %s",
                chat_id, rc, count, sqlite3_errmsg(db));
    if (st) sqlite3_finalize(st);
    u_free(files); return -1;
}

int media_index_generate_uuid(uint8_t uuid_out[16]) {
    return ma_uuid(uuid_out);
}

void media_index_result_free(struct media_index_result* result) {
    if (!result) return;
    if (result->block_ids) { u_free(result->block_ids); result->block_ids = NULL; }
    if (result->block_sigs) { u_free(result->block_sigs); result->block_sigs = NULL; }
}

/* Завершить только свой savepoint; SQLite могла уже отменить всю транзакцию. */
static int mi_end_registration(sqlite3* db, int failed) {
    if (!failed && !mi_exec(db, "RELEASE media_registration")) return 0;
    if (!sqlite3_get_autocommit(db)) {
        if (!mi_exec(db, "ROLLBACK TO media_registration")) mi_exec(db, "RELEASE media_registration");
    } else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media_index registration transaction no longer active");
    return -1;
}

/* Проверка геометрии без умножения непроверенных размеров и числа блоков. */
static int mi_result_valid(const struct media_index_result* result) {
    if (result && result->file_size > 0 && result->block_size > 0 && result->num_blocks > 0 && result->block_ids
        && (uint64_t)result->num_blocks <= UINT32_MAX / 64
        && (result->file_size - 1) / result->block_size + 1 == result->num_blocks) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index invalid geometry bytes=%lld block_size=%lld count=%d ids=%p",
                (long long)(result ? result->file_size : 0), (long long)(result ? result->block_size : 0),
                result ? result->num_blocks : 0, (void*)(result ? result->block_ids : NULL)); return -1;
}

static int mi_path_access_read(sqlite3* db, const char* location, const struct media_index_result* file) {
    sqlite3_stmt* st = NULL;
    const char* sql = file ? "SELECT 1 FROM media_files WHERE location=? AND (content_hash<>? OR file_size<>?) LIMIT 1" :
        "SELECT 1 FROM media_files WHERE location=? LIMIT 1";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, location, -1, SQLITE_STATIC);
    if (!rc && file) rc = sqlite3_bind_blob(st, 2, file->content_hash, 32, SQLITE_STATIC);
    if (!rc && file) rc = sqlite3_bind_int64(st, 3, file->file_size);
    if (!rc) {
        rc = sqlite3_step(st);
        rc = rc == SQLITE_DONE ? SQLITE_OK : rc == SQLITE_ROW ? SQLITE_CONSTRAINT : rc;
    }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media destination unavailable path=%s rc=%d", location, rc);
    return rc;
}
int media_index_path_access_read(sqlite3* db, const char* path, const char* base, const struct media_index_result* file) {
    char location[MEDIA_PATH_MAX];
    if (media_index_location(path, base, location)) return SQLITE_MISMATCH;
    return mi_path_access_read(db, location, file);
}

/* Только SQL: все блоки и снятие preparation входят в WRITE caller. */
int media_index_commit_write(sqlite3* db, const struct media_index_result* result,
    const uint8_t holder[32], const uint8_t* key, const char* chat_id, const char* dest_path, const char* media_base) {
    char location[MEDIA_PATH_MAX];
    if (!db || sqlite3_get_autocommit(db) || !holder || !memcmp(holder, (uint8_t[32]){0}, 32) ||
        !chat_id || !*chat_id || strlen(chat_id) > 64 || mi_result_valid(result)
        || media_index_location(dest_path, media_base, location)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index WRITE invalid registration/transaction"); return SQLITE_MISUSE;
    }
    int rc = mi_path_access_read(db, location, result);
    if (rc) return rc;
    int64_t ts = (int64_t)time(NULL);
    for (int n = 0; n < result->num_blocks; n++) {
        int64_t offset = (int64_t)n * result->block_size;
        int64_t size = n == result->num_blocks - 1 ? result->file_size - offset : result->block_size;
        uint8_t signature[64] = {0};
        if (key) {
            uint8_t message[116]; size_t used = 0;
            memcpy(message + used, result->media_id, 16); used += 16;
            memcpy(message + used, result->content_hash, 32); used += 32;
            memcpy(message + used, &ts, 8); used += 8;
            memcpy(message + used, &result->file_size, 8); used += 8;
            memcpy(message + used, &size, 8); used += 8;
            int32_t chunk = n; memcpy(message + used, &chunk, 4); used += 4;
            memcpy(message + used, &offset, 8); used += 8;
            memcpy(message + used, holder, 32); used += 32;
            if (ma_sign_block(key, message, used, signature)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index signing failed chat=%s chunk=%d", chat_id, n); return SQLITE_ERROR;
            }
        }
        rc = mi_insert_write(db, result->media_id, result->block_ids + (size_t)n * 16, result->content_hash,
            chat_id, location, holder, signature, ts, result->file_size, size, n, offset);
        if (rc) return rc;
    }
    sqlite3_stmt* st = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM media_preparations WHERE chat_id=? AND location=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, chat_id, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 2, location, -1, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index clearing preparation chat=%s rc=%d: %s", chat_id, rc, sqlite3_errstr(rc));
    else DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media_index registration staged chat=%s blocks=%d path=%s signed=%d",
        chat_id, result->num_blocks, location, key != NULL);
    return rc;
}
int media_index_commit(sqlite3* db, const struct media_index_result* result,
    const uint8_t holder[32], const uint8_t* key, const char* chat_id, const char* dest_path, const char* media_base) {
    if (!db || mi_exec(db, "SAVEPOINT media_registration")) return -1;
    int rc = media_index_commit_write(db, result, holder, key, chat_id, dest_path, media_base);
    if (mi_end_registration(db, !!rc)) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media_index registered chat=%s blocks=%d path=%s signed=%d",
        chat_id, result->num_blocks, dest_path, key != NULL); return 0;
}
int media_index_register_downloaded(sqlite3* db, const uint8_t* media_id, const uint8_t* block_id,
                                    const uint8_t* content_hash, const char* chat_id,
                                    const char* location, const uint8_t holder[32],
                                    int64_t file_size, int64_t chunk_size, int chunk, int64_t offset) {
    if (!db || !media_id || !block_id || !content_hash || !holder || !memcmp(holder, (uint8_t[32]){0}, 32) ||
        !chat_id || !*chat_id || strlen(chat_id) > 64
        || mi_location_valid(location) || file_size <= 0 || chunk_size <= 0 || chunk < 0
        || offset < 0 || offset >= file_size || chunk_size > file_size - offset) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index invalid downloaded block"); return -1;
    }
    static const uint8_t zero_sign[64] = {0};
    return mi_insert_write(db, media_id, block_id, content_hash, chat_id, location,
        holder, zero_sign, (int64_t)time(NULL), file_size, chunk_size, chunk, offset) ? -1 : 0;
}

/* ─── register_async (worker + uasync callback) ─── */

/* Uasync: RW всегда использует owner; RAM/переходный readonly reader оставляют один legacy writer. */
static void mi_sql_submit(struct db_async* owner, sqlite3* legacy, const char* name, db_async_work_fn work,
    void* data, db_async_done_fn done, db_async_guard_fn guard) {
    if (db_async_can_write(owner)) { db_async_submit_guarded(owner, name, DB_ASYNC_WRITE, guard, work, data, done); return; }
    const char* file = legacy ? sqlite3_db_filename(legacy, "main") : NULL;
    int allowed = legacy && (owner || (file && !*file));
    int rc = !allowed || !sqlite3_get_autocommit(legacy) ? SQLITE_MISUSE : guard && !guard(data) ? SQLITE_ABORT :
        sqlite3_exec(legacy, "BEGIN IMMEDIATE /* media index */", NULL, NULL, NULL);
    int owns = !rc;
    if (!rc) rc = work(legacy, data);
    if (!rc) rc = sqlite3_exec(legacy, "COMMIT /* media index */", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s legacy rc=%d: %s", name, rc, sqlite3_errstr(rc));
        if (owns && !sqlite3_get_autocommit(legacy)) {
            int rollback = sqlite3_exec(legacy, "ROLLBACK /* media index */", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s legacy rollback rc=%d: %s", name, rollback, sqlite3_errmsg(legacy));
            if (!sqlite3_get_autocommit(legacy)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s legacy transaction remains open; caller must stop database owner", name); rc = SQLITE_IOERR;
            }
        }
    }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    snprintf(result.message, sizeof(result.message), "%s legacy: %s", name, sqlite3_errstr(rc)); done(data, &result);
}

struct mi_track_ctx {
    char chat[65], path[MEDIA_PATH_MAX], base[MEDIA_BASE_MAX];
    db_async_done_fn done;
    void* arg;
};
static int mi_track_work(sqlite3* db, void* arg) {
    struct mi_track_ctx* ctx = arg;
    int rc = media_index_path_access_read(db, ctx->path, ctx->base, NULL);
    return rc ? rc : media_index_track_preparation_write(db, ctx->chat, ctx->path, ctx->base);
}
static void mi_track_done(void* arg, const struct db_async_result* result) {
    struct mi_track_ctx* ctx = arg;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media track result chat=%s status=%d rc=%d: %s",
        ctx->chat, result->status, result->sqlite_code, result->message);
    ctx->done(ctx->arg, result); u_free(ctx);
}
void media_index_track_preparation_async(struct db_async* owner, sqlite3* legacy, const char* chat,
    const char* path, const char* base, db_async_done_fn done, void* arg) {
    char location[MEDIA_PATH_MAX]; int rc = SQLITE_MISUSE;
    if (!done || !chat || !*chat || strlen(chat) > 64 || media_index_location(path, base, location) ||
        strlen(path) >= MEDIA_PATH_MAX || strlen(base) >= MEDIA_BASE_MAX) goto fail;
    struct mi_track_ctx* ctx = u_calloc(1, sizeof(*ctx)); rc = SQLITE_NOMEM; if (!ctx) goto fail;
    snprintf(ctx->chat, sizeof(ctx->chat), "%s", chat); snprintf(ctx->path, sizeof(ctx->path), "%s", path);
    snprintf(ctx->base, sizeof(ctx->base), "%s", base); ctx->done = done; ctx->arg = arg;
    mi_sql_submit(owner, legacy, "media-track", mi_track_work, ctx, mi_track_done, NULL); return;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media track request invalid/OOM rc=%d", rc);
    struct db_async_result result = {.status=rc == SQLITE_NOMEM ? DB_ASYNC_NOMEM : DB_ASYNC_INVALID, .sqlite_code=rc};
    if (done) done(arg, &result);
}

struct mi_reg_ctx {
    struct UASYNC* ua;
    struct media_async* ma; /* Только uasync после committed track; valid запрещает I/O после stop. */
    struct db_async* worker;
    int (*valid)(void*);
    int sql_phase, use_async; /* 0=track до I/O,1=commit готовых блоков; legacy API использует прежний callback. */
    uint8_t  ed25519_privkey[32];
    char     src_path[1024];
    char     dest_path[1024];
    int      copy_file;

    uint8_t holder[32];
    char     chat_id[65];
    char     media_base[512];

    sqlite3* db;
    void (*cb)(void* arg, int err, const struct media_index_result* result);
    void*    cb_arg;

    struct media_index_result result;
    int err;
};
static void mi_reg_sql_done(void*, const struct db_async_result*);
static int mi_reg_current(void* arg) {
    struct mi_reg_ctx* ctx = arg; return !ctx->valid || ctx->valid(ctx->cb_arg);
}
static int mi_reg_sql_work(sqlite3* db, void* arg) {
    struct mi_reg_ctx* ctx = arg;
    return ctx->sql_phase ? media_index_commit_write(db, &ctx->result, ctx->holder, ctx->ed25519_privkey,
        ctx->chat_id, ctx->dest_path, ctx->media_base) :
        media_index_track_preparation_write(db, ctx->chat_id, ctx->dest_path, ctx->media_base);
}
static void mi_reg_finish(struct mi_reg_ctx* ctx) {
    if (ctx->cb) ctx->cb(ctx->cb_arg, ctx->err, ctx->err ? NULL : &ctx->result);
    media_index_result_free(&ctx->result); OPENSSL_cleanse(ctx->ed25519_privkey, sizeof(ctx->ed25519_privkey)); u_free(ctx);
}

/* Только полная подготовка допускает commit; любая ошибка оставляет result для обычной очистки done. */
static void mi_reg_work(void* data) {
    struct mi_reg_ctx* ctx = data;
    FILE* file = NULL; uint8_t* buffer = NULL;
    const char* phase = "copy";
    ctx->err = -10;
    if (ctx->copy_file && ma_copy_file(ctx->src_path, ctx->dest_path)) goto done;
    phase = "size"; ctx->err = -20;
    int64_t size = ma_file_size(ctx->dest_path);
    if (size <= 0) goto done;
    int64_t block = media_calc_block_size((uint64_t)size);
    uint64_t count = (uint64_t)(size / block + (size % block != 0));
    phase = "block count"; ctx->err = -21;
    if (count > INT_MAX || count > UINT32_MAX / 64) goto done;
    ctx->result.file_size = size; ctx->result.block_size = block; ctx->result.num_blocks = (int)count;
    phase = "hash"; ctx->err = -30;
    if (ma_sha256_file(ctx->dest_path, ctx->result.content_hash)) goto done;
    phase = "media UUID"; ctx->err = -31;
    if (ma_uuid(ctx->result.media_id)) goto done;
    phase = "block arrays"; ctx->err = -40;
    ctx->result.block_ids = u_malloc((uint32_t)count * 16);
    ctx->result.block_sigs = u_malloc((uint32_t)count * 64);
    if (!ctx->result.block_ids || !ctx->result.block_sigs) goto done;
    phase = "open"; ctx->err = -50;
    file = file_io_open_read(ctx->dest_path);
    if (!file) goto done;
    for (int n = 0; n < ctx->result.num_blocks; n++) {
        int64_t offset = (int64_t)n * block;
        size_t length = (size_t)(n == ctx->result.num_blocks - 1 ? size - offset : block);
        phase = "block allocation"; ctx->err = -60;
        buffer = u_malloc((uint32_t)length);
        if (!buffer) goto done;
        phase = "seek"; ctx->err = -70;
#ifdef _WIN32
        int seek_error = _fseeki64(file, offset, SEEK_SET);
#else
        int seek_error = (int64_t)(off_t)offset != offset || fseeko(file, (off_t)offset, SEEK_SET);
#endif
        if (seek_error) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index seek failed path=%s offset=%lld errno=%d",
                        ctx->dest_path, (long long)offset, errno); goto done;
        }
        phase = "read";
        size_t received = fread(buffer, 1, length, file);
        if (received != length) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index read failed path=%s offset=%lld wanted=%zu received=%zu error=%d",
                        ctx->dest_path, (long long)offset, length, received, ferror(file)); goto done;
        }
        phase = "block UUID"; ctx->err = -81;
        if (ma_uuid(ctx->result.block_ids + (size_t)n * 16)) goto done;
        phase = "block signature"; ctx->err = -80;
        if (ma_sign_block(ctx->ed25519_privkey, buffer, length, ctx->result.block_sigs + (size_t)n * 64)) goto done;
        u_free(buffer); buffer = NULL;
    }
    phase = "close"; ctx->err = -71;
    int close_error = fclose(file); file = NULL;
    if (close_error) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index close failed path=%s errno=%d", ctx->dest_path, errno); goto done; }
    phase = "durable file"; ctx->err = -72;
    if (file_io_sync_file(ctx->dest_path) || file_io_sync_directory(ctx->dest_path)) goto done;
    ctx->err = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media_index prepared path=%s bytes=%lld blocks=%d",
                ctx->dest_path, (long long)size, ctx->result.num_blocks);
done:
    if (ctx->err) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index preparation failed phase=%s path=%s error=%d",
                              phase, ctx->dest_path, ctx->err);
    u_free(buffer);
    if (file && fclose(file)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media_index cleanup close failed path=%s errno=%d", ctx->dest_path, errno);
}

static void mi_reg_done(void* arg, int err) {
    struct mi_reg_ctx* ctx = (struct mi_reg_ctx*)arg;
    if (err) ctx->err = err;
    if (ctx->use_async) {
        if (!mi_reg_current(ctx) && !ctx->err) ctx->err = -SQLITE_ABORT;
        if (ctx->err) { mi_reg_finish(ctx); return; }
        ctx->sql_phase = 1;
        mi_sql_submit(ctx->worker, ctx->db, "media-register", mi_reg_sql_work, ctx, mi_reg_sql_done, mi_reg_current); return;
    }

    if (ctx->err == 0) {
        int rc = media_index_commit(ctx->db, &ctx->result, ctx->holder,
                                     ctx->ed25519_privkey, ctx->chat_id,
                                     ctx->dest_path, ctx->media_base);
        if (rc != 0) ctx->err = -90;
    }

    mi_reg_finish(ctx);
}

static void mi_reg_sql_done(void* arg, const struct db_async_result* result) {
    struct mi_reg_ctx* ctx = arg; int current = mi_reg_current(ctx);
    if (result->status || !current) {
        ctx->err = result->sqlite_code ? -result->sqlite_code : result->status ? result->status : -SQLITE_ABORT;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media registration SQL chat=%s phase=%d current=%d status=%d rc=%d: %s",
            ctx->chat_id, ctx->sql_phase, current, result->status, result->sqlite_code, result->message);
        mi_reg_finish(ctx); return;
    }
    if (!ctx->sql_phase) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media preparation durable chat=%s path=%s; starting I/O", ctx->chat_id, ctx->dest_path);
        media_async_submit(ctx->ma, ctx->ua, mi_reg_work, ctx, mi_reg_done, ctx); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media registration committed chat=%s blocks=%d path=%s",
        ctx->chat_id, ctx->result.num_blocks, ctx->dest_path); mi_reg_finish(ctx);
}

void media_index_register_db_async(struct media_async* ma, struct UASYNC* ua, struct db_async* owner, sqlite3* legacy,
    const uint8_t holder[32], const uint8_t* key, const char* chat, const char* source, const char* destination, int copy,
    const char* base, int (*valid)(void*), void (*done)(void*, int, const struct media_index_result*), void* arg) {
    char location[MEDIA_PATH_MAX];
    if (!ma || !ua || !done || !holder || !memcmp(holder, (uint8_t[32]){0}, 32) || !key || !chat || !*chat || strlen(chat) > 64 || !source || !*source ||
        strlen(source) >= MEDIA_PATH_MAX || media_index_location(destination, base, location) ||
        strlen(destination) >= MEDIA_PATH_MAX || strlen(base) >= MEDIA_BASE_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media registration request invalid"); if (done) done(arg, -SQLITE_MISUSE, NULL); return;
    }
    struct mi_reg_ctx* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media registration context OOM"); done(arg, -SQLITE_NOMEM, NULL); return; }
    ctx->ua = ua; ctx->ma = ma; ctx->worker = owner; ctx->db = legacy; ctx->use_async = 1;
    ctx->valid = valid; ctx->cb = done; ctx->cb_arg = arg; ctx->copy_file = !!copy; memcpy(ctx->holder, holder, 32);
    memcpy(ctx->ed25519_privkey, key, 32); snprintf(ctx->chat_id, sizeof(ctx->chat_id), "%s", chat);
    snprintf(ctx->src_path, sizeof(ctx->src_path), "%s", source); snprintf(ctx->dest_path, sizeof(ctx->dest_path), "%s", destination);
    snprintf(ctx->media_base, sizeof(ctx->media_base), "%s", base);
    mi_sql_submit(owner, legacy, "media-track", mi_reg_sql_work, ctx, mi_reg_sql_done, mi_reg_current);
}

void media_index_register_async(
    struct media_async* ma, struct UASYNC* ua, sqlite3* db,
    const uint8_t holder[32], const uint8_t* ed25519_privkey,
    const char* chat_id,
    const char* src_path, const char* dest_path, int copy_file,
    const char* media_base,
    void (*cb)(void* arg, int err, const struct media_index_result* result),
    void* cb_arg) {

    if (!ma || !ua || !db || !holder || !memcmp(holder, (uint8_t[32]){0}, 32) || !ed25519_privkey || !chat_id || !src_path || !dest_path || !media_base || !cb) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index: register_async invalid args");
        if (cb) cb(cb_arg, -1, NULL);
        return;
    }

    char checked_location[MEDIA_PATH_MAX];
    if (!*src_path || !*chat_id || media_index_location(dest_path, media_base, checked_location)
        || strlen(src_path) >= sizeof(((struct mi_reg_ctx*)0)->src_path)
        || strlen(dest_path) >= sizeof(((struct mi_reg_ctx*)0)->dest_path)
        || strlen(chat_id) >= sizeof(((struct mi_reg_ctx*)0)->chat_id)
        || strlen(media_base) >= sizeof(((struct mi_reg_ctx*)0)->media_base)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index register oversized/empty path"); cb(cb_arg, -1, NULL); return;
    }
    if (media_index_track_preparation(db, chat_id, dest_path, media_base)) { cb(cb_arg, -1, NULL); return; }
    struct mi_reg_ctx* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media_index context allocation failed"); cb(cb_arg, -1, NULL); return; }

    ctx->ua   = ua;
    ctx->db   = db;
    memcpy(ctx->holder, holder, 32);
    ctx->copy_file = copy_file;
    ctx->cb   = cb;
    ctx->cb_arg = cb_arg;

    memcpy(ctx->ed25519_privkey, ed25519_privkey, 32);
    snprintf(ctx->src_path, sizeof(ctx->src_path), "%s", src_path);
    snprintf(ctx->dest_path, sizeof(ctx->dest_path), "%s", dest_path);
    snprintf(ctx->chat_id, sizeof(ctx->chat_id), "%s", chat_id);
    snprintf(ctx->media_base, sizeof(ctx->media_base), "%s", media_base);

    media_async_submit(ma, ua, mi_reg_work, ctx, mi_reg_done, ctx);
}
