#include "store.h"
#include "../chat/chat_core.h"
#include "../transport_layer/secure_channel.h"
#include "../db_schema.h"
#include "../../lib/debug_config.h"
#include <limits.h>
#include <string.h>

static int nonzero(const uint8_t* id, size_t size) {
    unsigned value = 0; for (size_t i = 0; i < size; i++) value |= id[i]; return value != 0;
}
static int owner_valid(enum attachment_owner owner) { return owner >= ATTACHMENT_OWNER_CHAT && owner <= ATTACHMENT_OWNER_DM; }
static enum attachment_scope owner_scope(enum attachment_owner owner) {
    return owner == ATTACHMENT_OWNER_DM ? ATTACHMENT_SCOPE_DM : ATTACHMENT_SCOPE_GROUP;
}
/* Сохранить исходную ошибку до finalize, который может сбросить error connection. */
static int finish(sqlite3* db, sqlite3_stmt* st, int code, const char* stage) {
    if (code == SQLITE_DONE) code = SQLITE_OK;
    if (code) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment store stage=%s rc=%d (%s): %s",
        stage, code, sqlite3_errstr(code), db ? sqlite3_errmsg(db) : "no database");
    int final = sqlite3_finalize(st);
    if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment store finalize stage=%s rc=%d (%s)", stage, final, sqlite3_errstr(final));
    return code ? code : final;
}
static int write_valid(sqlite3* db) {
    if (db && !sqlite3_get_autocommit(db)) return 1;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment store WRITE requires caller transaction"); return 0;
}
static int namespace_valid(const char* space, int is_dm) {
    return is_dm ? chat_numeric_id(space) != 0 : chat_channel_valid(space);
}
static int object_valid(const struct attachment_object* object) {
    return object && namespace_valid(object->space, object->scope == ATTACHMENT_SCOPE_DM) && nonzero(object->author, 32) && object->size <= INT64_MAX &&
        object->manifest && object->manifest_size && object->manifest_size <= ATTACHMENT_MANIFEST_MAX &&
        ((object->scope == ATTACHMENT_SCOPE_GROUP &&
          (object->crypto == ATTACHMENT_CRYPTO_NONE || object->crypto == ATTACHMENT_CRYPTO_GROUP)) ||
         (object->scope == ATTACHMENT_SCOPE_DM && object->crypto == ATTACHMENT_CRYPTO_DM));
}
static int reference_valid(const struct attachment_ref* ref) {
    return ref && owner_valid(ref->owner) && namespace_valid(ref->space, ref->owner == ATTACHMENT_OWNER_DM) && nonzero(ref->message, 32);
}

int attachment_store_prepare_write(sqlite3* db) {
    if (!write_valid(db)) return SQLITE_MISUSE;
    const char* tables[] = {"attachment_objects", "attachment_refs", "attachment_jobs"};
    for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        int code = db_schema_table_ex(db, tables[i], NULL); if (code) return code;
    }
    if (db_schema_attachment_views(db)) return SQLITE_SCHEMA;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment store schema prepared; caller commit pending"); return SQLITE_OK;
}

int attachment_store_begin_write(sqlite3* db, const struct attachment_job* job) {
    if (!write_valid(db) || !job || !owner_valid(job->owner) || !namespace_valid(job->space, job->owner == ATTACHMENT_OWNER_DM) || !nonzero(job->author, 32) || !nonzero(job->id, 16) ||
        !nonzero(job->namespace_token, 32) || !job->source || !*job->source || strlen(job->source) >= ATTACHMENT_PATH_MAX ||
        !job->request || !job->request_size || job->request_size > ATTACHMENT_REQUEST_MAX)
        return finish(db, NULL, SQLITE_MISUSE, "begin arguments");
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO attachment_jobs(id,owner,namespace,namespace_token,author,source,request,state)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,1) ON CONFLICT(id) DO NOTHING", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 2, job->owner);
    if (!rc) rc = sqlite3_bind_text(st, 3, job->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, job->namespace_token, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 5, job->author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 6, job->source, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 7, job->request, (int)job->request_size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    rc = finish(db, st, rc, "begin insert"); if (rc) return rc;
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_jobs WHERE id=?1 AND owner=?2 AND namespace=?3 AND"
        " namespace_token=?4 AND author=?5 AND source=?6 AND request=?7", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 2, job->owner);
    if (!rc) rc = sqlite3_bind_text(st, 3, job->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, job->namespace_token, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 5, job->author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 6, job->source, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 7, job->request, (int)job->request_size, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = SQLITE_OK; else if (rc == SQLITE_DONE) rc = SQLITE_CONSTRAINT; }
    rc = finish(db, st, rc, "begin immutable intent");
    if (!rc) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment job accepted owner=%d space=%s author=%s; commit pending",
        job->owner, job->space, sc_public_key_hex(job->author, (char[65]){0}));
    return rc;
}

/* Регистрация не меняет описание существующего UUID и не трогает его локальную копию. */
static int register_object(sqlite3* db, const struct attachment_object* object, int* conflict) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO attachment_objects(scope,namespace,id,author,size,hash,crypto,manifest)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8) ON CONFLICT(scope,namespace,id) DO NOTHING", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int(st, 1, object->scope);
    if (!rc) rc = sqlite3_bind_text(st, 2, object->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, object->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, object->author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 5, (sqlite3_int64)object->size);
    if (!rc) rc = sqlite3_bind_blob(st, 6, object->hash, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 7, object->crypto);
    if (!rc) rc = sqlite3_bind_blob(st, 8, object->manifest, (int)object->manifest_size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    rc = finish(db, st, rc, "object insert"); if (rc) return rc;
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_objects WHERE scope=?1 AND namespace=?2 AND id=?3"
        " AND author=?4 AND size=?5 AND hash=?6 AND crypto=?7 AND manifest=?8", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int(st, 1, object->scope);
    if (!rc) rc = sqlite3_bind_text(st, 2, object->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, object->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, object->author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 5, (sqlite3_int64)object->size);
    if (!rc) rc = sqlite3_bind_blob(st, 6, object->hash, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 7, object->crypto);
    if (!rc) rc = sqlite3_bind_blob(st, 8, object->manifest, (int)object->manifest_size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && conflict) {
        rc = finish(db, st, rc, "object immutable manifest");
        if (rc) return rc;
        *conflict = 1;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment object description conflicts with registered UUID scope=%d space=%s",
            object->scope, object->space);
        return SQLITE_OK;
    }
    if (rc == SQLITE_ROW) rc = SQLITE_OK;
    else if (rc == SQLITE_DONE) rc = SQLITE_CONSTRAINT;
    return finish(db, st, rc, "object immutable manifest");
}

int attachment_store_ready_write(sqlite3* db, const uint8_t job[16], const struct attachment_object* object, const char* path) {
    if (!write_valid(db) || !job || !nonzero(job, 16) || !object_valid(object) || !path || !*path || strlen(path) >= ATTACHMENT_PATH_MAX)
        return finish(db, NULL, SQLITE_MISUSE, "ready arguments");
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_jobs WHERE id=?1 AND namespace=?2 AND author=?3"
        " AND CASE owner WHEN 3 THEN 2 ELSE 1 END=?4 AND (state=1 OR"
        " (state IN(2,3) AND object=?5 AND local_path=?6))", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 2, object->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, object->author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 4, object->scope);
    if (!rc) rc = sqlite3_bind_blob(st, 5, object->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 6, path, -1, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = SQLITE_OK; else if (rc == SQLITE_DONE) rc = SQLITE_CONSTRAINT; }
    rc = finish(db, st, rc, "ready job identity"); if (rc) return rc;
    rc = register_object(db, object, NULL); if (rc) return rc;
    rc = sqlite3_prepare_v2(db, "UPDATE attachment_jobs SET state=2,object=?2,local_path=?3 WHERE id=?1 AND state=1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, object->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 3, path, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    rc = finish(db, st, rc, "ready transition");
    if (!rc) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment file ready scope=%d space=%s bytes=%llu; commit pending",
        object->scope, object->space, (unsigned long long)object->size);
    return rc;
}

static int bind_reference(sqlite3* db, const struct attachment_ref* ref) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO attachment_refs(owner,scope,namespace,message,object,released)"
        " VALUES(?1,?2,?3,?4,?5,0) ON CONFLICT(owner,namespace,message,object) DO NOTHING", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int(st, 1, ref->owner);
    if (!rc) rc = sqlite3_bind_int(st, 2, owner_scope(ref->owner));
    if (!rc) rc = sqlite3_bind_text(st, 3, ref->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, ref->message, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 5, ref->object, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    return finish(db, st, rc, "bind message reference");
}

int attachment_store_publish_write(sqlite3* db, const uint8_t job[16], const struct attachment_ref* ref) {
    if (!write_valid(db) || !job || !nonzero(job, 16) || !reference_valid(ref))
        return finish(db, NULL, SQLITE_MISUSE, "publish arguments");
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_jobs WHERE id=?1 AND owner=?2 AND namespace=?3 AND object=?4"
        " AND (state=2 OR (state=3 AND message=?5))", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 2, ref->owner);
    if (!rc) rc = sqlite3_bind_text(st, 3, ref->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, ref->object, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 5, ref->message, 32, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = SQLITE_OK; else if (rc == SQLITE_DONE) rc = SQLITE_CONSTRAINT; }
    rc = finish(db, st, rc, "publish job identity"); if (rc) return rc;
    rc = bind_reference(db, ref); if (rc) return rc;
    rc = sqlite3_prepare_v2(db, "UPDATE attachment_jobs SET state=3,message=?2 WHERE id=?1 AND state=2", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, ref->message, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    rc = finish(db, st, rc, "publish transition");
    if (!rc) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment reference published owner=%d space=%s; commit pending",
        ref->owner, ref->space);
    return rc;
}

int attachment_store_observe_write(sqlite3* db, const struct attachment_object* object, const struct attachment_ref* ref, int* conflict) {
    if (conflict) *conflict = 0;
    if (!write_valid(db) || !object_valid(object) || !reference_valid(ref) || object->scope != owner_scope(ref->owner) ||
        strcmp(object->space, ref->space) || memcmp(object->id, ref->object, 16))
        return finish(db, NULL, SQLITE_MISUSE, "observe arguments");
    int rc = register_object(db, object, conflict);
    if (rc || (conflict && *conflict)) return rc;
    rc = bind_reference(db, ref);
    if (!rc) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "verified attachment observed owner=%d space=%s; commit pending",
        ref->owner, ref->space);
    return rc;
}

int attachment_store_release_write(sqlite3* db, const struct attachment_ref* ref) {
    if (!write_valid(db) || !reference_valid(ref)) return finish(db, NULL, SQLITE_MISUSE, "release arguments");
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE attachment_refs SET released=1 WHERE owner=?1 AND namespace=?2 AND message=?3 AND object=?4",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int(st, 1, ref->owner);
    if (!rc) rc = sqlite3_bind_text(st, 2, ref->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, ref->message, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, ref->object, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    rc = finish(db, st, rc, "release reference");
    if (!rc) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment reference released owner=%d space=%s; object retained",
        ref->owner, ref->space);
    return rc;
}

int attachment_store_live_reference_read(sqlite3* db, const struct attachment_ref* ref) {
    if (!db || !reference_valid(ref)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment reference READ: invalid reference"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_refs WHERE owner=? AND namespace=? AND message=? AND object=? AND released=0", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int(st, 1, ref->owner);
    if (!rc) rc = sqlite3_bind_text(st, 2, ref->space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, ref->message, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, ref->object, 16, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); rc = rc == SQLITE_ROW ? SQLITE_OK : rc == SQLITE_DONE ? SQLITE_NOTFOUND : rc; }
    return finish(db, st, rc, "live reference");
}

int attachment_store_job_read(sqlite3* db, const uint8_t job[16], enum attachment_job_state* state, uint8_t message[32]) {
    if (state) *state = 0; if (message) memset(message, 0, 32);
    if (!db || !job || !state || !message) return finish(db, NULL, SQLITE_MISUSE, "job READ arguments");
    sqlite3_stmt* st = NULL; enum attachment_job_state value = 0; uint8_t id[32] = {0};
    int rc = sqlite3_prepare_v2(db, "SELECT state,message FROM attachment_jobs WHERE id=?1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        value = (enum attachment_job_state)sqlite3_column_int(st, 0);
        const void* bytes = sqlite3_column_blob(st, 1); int length = sqlite3_column_bytes(st, 1);
        if (value < ATTACHMENT_JOB_PREPARING || value > ATTACHMENT_JOB_CANCELLED ||
            (value == ATTACHMENT_JOB_PUBLISHED ? !bytes || length != 32 : sqlite3_column_type(st, 1) != SQLITE_NULL)) rc = SQLITE_CORRUPT;
        else { if (bytes) memcpy(id, bytes, 32); rc = SQLITE_OK; }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    rc = finish(db, st, rc, "job READ"); if (!rc) { *state = value; memcpy(message, id, 32); }
    return rc;
}

int attachment_store_authorize_read(sqlite3* db, const uint8_t job[16], enum attachment_owner owner,
    const char* space, const uint8_t author[32], const uint8_t token[32]) {
    if (!db || !job || !token || !owner_valid(owner) || !namespace_valid(space, owner == ATTACHMENT_OWNER_DM) || !author || !nonzero(author, 32))
        return finish(db, NULL, SQLITE_MISUSE, "authorize arguments");
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_jobs WHERE id=?1 AND owner=?2 AND namespace=?3 AND author=?4"
        " AND namespace_token=?5 AND state IN(2,3)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 2, owner);
    if (!rc) rc = sqlite3_bind_text(st, 3, space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 5, token, 32, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = SQLITE_OK; else if (rc == SQLITE_DONE) rc = SQLITE_AUTH; }
    return finish(db, st, rc, "authorize publisher namespace");
}

int attachment_store_public_read(sqlite3* db, const char* space, const uint8_t object[16]) {
    if (!db || !chat_channel_valid(space) || !object) return -finish(db, NULL, SQLITE_MISUSE, "public object arguments");
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_public_objects WHERE namespace=?1 AND id=?2", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, object, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW;
    if (found || rc == SQLITE_DONE) rc = SQLITE_OK;
    rc = finish(db, st, rc, "public object READ");
    return rc ? -rc : found;
}
