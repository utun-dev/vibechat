#include "download.h"
#include "service_priv.h"
#include "manifest.h"
#include "../chat/chat_core_priv.h"
#include "../chat/chat_attachment.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../media_delivery/media_download.h"
#include "../media_async/media_async.h"
#include "../media_async/attachment.h"
#include "../../lib/db_async.h"
#include "../../lib/file_io.h"
#include "../../lib/mem.h"
#include <string.h>

struct group_download {
    struct group_download* next;
    struct attachment_ref ref;
    char group[65], base[512], path[1024];
    uint8_t self[32], author[32];
    struct media_index_result file;
    int local, indexed, cancelled, network, path_ready;
    struct UTUN_INSTANCE* inst; /* Только uasync, не SQL/file worker. */
    struct attachment_service* runtime;
    uint64_t generation;
    void (*done)(void*, int, const char*); void* arg;
    void (*progress)(void*, int, int);
};
/* Имя остаётся basename; UUID исключает коллизии, расширение сохраняет обычное открытие файла в GUI. */
static void download_name(const char* body, const char* type, char name[256]) {
    const char* standard = !strcmp(type, "video/mp4") ? "attachment.mp4" : !strcmp(type, "video/x-matroska") ? "attachment.mkv" :
        !strcmp(type, "image/jpeg") ? "attachment.jpg" : !strcmp(type, "image/png") ? "attachment.png" :
        !strcmp(type, "image/webp") ? "attachment.webp" : !strcmp(type, "image/bmp") ? "attachment.bmp" : "attachment.file";
    strcpy(name, standard);
    if (strcmp(type, "application/octet-stream")) return;
    const char* end = strchr(body, '|'); if (!end) return;
    struct attachment_info info = {.kind=ATTACHMENT_FILE}; size_t length = (size_t)(end - body);
    if (*body == '*') {
        if (length > 344 + 1) return;
        length = b64_decode(body + 1, length - 1, (uint8_t*)info.name, sizeof(info.name));
    } else if (length < sizeof(info.name)) memcpy(info.name, body, length);
    if (!length || length >= sizeof(info.name) || memchr(info.name, 0, length)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "group attachment download uses generated name: invalid filename bytes=%zu", length); return;
    }
    info.name[length] = 0;
    if (!attachment_validate(&info)) strcpy(name, info.name);
    else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "group attachment download uses generated name: unsafe basename");
}
int attachment_group_filename(const uint8_t media[16], const char* body, const char* type, char name[256]) {
    uint8_t cipher[16]; char hex[33], label[256];
    if (!strcmp(type, ENCRYPTED_ATTACHMENT_TYPE)) {
        if (chat_attachment_identity(body, strlen(body), cipher)) return -1;
        media = cipher;
    }
    for (unsigned i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", media[i]);
    if (!strcmp(type, ENCRYPTED_ATTACHMENT_TYPE)) snprintf(name, 256, "%s.uenc", hex);
    else if (!strcmp(type, "audio/opus")) snprintf(name, 256, "voice_%s.opus", hex);
    else {
        download_name(body, type, label);
        const char* ext = strrchr(label, '.');
        if (!ext || !ext[1] || strlen(ext) > 13 ||
            strspn(ext + 1, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != strlen(ext + 1)) ext = ".file";
        snprintf(name, 256, "%s%s", hex, ext);
    }
    return 0;
}

static int current(void* data) {
    struct group_download* download = data;
    return !download->inst->destroying && download->inst->attachments == download->runtime && attachment_service_workers(download->inst) &&
        chat_core_generation(download->inst) == download->generation && download->inst->md.initialized && !download->cancelled;
}
static void finished(void* data, int error) {
    struct group_download* download = data;
    if (download->cancelled) error = -2;
    else if (!current(download)) error = -1;
    if (download->runtime && download->inst->attachments == download->runtime) {
        struct group_download** slot = &download->runtime->downloads;
        while (*slot && *slot != download) slot = &(*slot)->next;
        if (*slot) *slot = download->next;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "group attachment download owner=%d ch=%s bytes=%lld local=%d error=%d", download->ref.owner,
        download->group, (long long)download->file.file_size, download->local, error);
    download->done(download->arg, error, error ? NULL : download->path);
    media_index_result_free(&download->file); u_free(download);
}

void attachment_group_download_detach(struct UTUN_INSTANCE* inst) {
    struct group_download* requests = inst->attachments->downloads;
    inst->attachments->downloads = NULL;
    for (struct group_download* d = requests; d; d = d->next) { d->runtime = NULL; d->cancelled = 1; }
}
static int access_reference(sqlite3* db, void* data) {
    struct group_download* d = data;
    uint64_t local; int error = SQLITE_OK;
    int member = topo_node_sqlite_member_local_id(db, d->group, d->self, 1, &local, &error);
    if (member) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "group file access denied ch=%s member=%d rc=%d", d->group, member, error);
        return error ? error : SQLITE_AUTH;
    }
    return attachment_store_live_reference_read(db, &d->ref);
}
/* Один SQL-снимок связывает допуск requester, живую ссылку и неизменяемое описание объекта. */
static int lookup(sqlite3* db, void* data) {
    struct group_download* download = data;
    int rc = access_reference(db, data); if (rc) return rc;
    sqlite3_stmt* st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT json_extract(CAST(o.manifest AS TEXT),'$.d'),o.author,"
        "(SELECT location FROM media_public_files f WHERE f.chat_id=o.namespace AND f.media_id=o.id AND f.holder=?1 AND f.chunk=0 AND f.offset=0 LIMIT 1),"
        "json_extract(CAST(o.manifest AS TEXT),'$.ct'),o.hash,o.size"
        " FROM attachment_objects o WHERE o.scope=1 AND o.namespace=?2 AND o.id=?3 AND (o.crypto=0 OR ?4=1)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, download->self, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 2, download->ref.space, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, download->ref.object, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 4, download->ref.owner == ATTACHMENT_OWNER_CHAT);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* body = (const char*)sqlite3_column_text(st, 0); const char* path = (const char*)sqlite3_column_text(st, 2);
        const uint8_t* ed = sqlite3_column_blob(st, 1); uint8_t x[32];
        const void* hash = sqlite3_column_blob(st, 4);
        if (!body || strlen(body) != (size_t)sqlite3_column_bytes(st, 0)) rc = SQLITE_CORRUPT;
        else if (attachment_manifest_parse(body, &download->file) ||
            !hash || sqlite3_column_bytes(st, 4) != 32 || memcmp(hash, download->file.content_hash, 32) ||
            download->file.file_size != sqlite3_column_int64(st, 5) || memcmp(download->file.media_id, download->ref.object, 16))
            rc = SQLITE_CORRUPT;
        else if (!ed || sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 32 ||
            sc_identity_public_x("group attachment author", ed, x) != SC_OK) rc = SQLITE_CORRUPT;
        else {
            memcpy(download->author, ed, sizeof(download->author));
            rc = media_index_base_read(db, download->base);
            download->indexed = path != NULL;
            if (!rc && path) rc = media_index_path(download->base, path, download->path) ? SQLITE_MISMATCH : SQLITE_OK;
            if (!rc && !path) {
                const char* type = (const char*)sqlite3_column_text(st, 3); char name[256], relative[384];
                if (!type || attachment_group_filename(download->ref.object, body, type, name)) rc = SQLITE_MISMATCH;
                else {
                    snprintf(relative, sizeof(relative), "media/%s/%s", download->group, name);
                    rc = media_index_path(download->base, relative, download->path) ? SQLITE_MISMATCH : SQLITE_OK;
                }
            }
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group file READ owner=%d ch=%s rc=%d: %s", download->ref.owner,
        download->group, rc, sqlite3_errmsg(db)); return rc;
}
static void local_check(void* data) {
    struct group_download* download = data; uint8_t hash[32];
    download->local = ma_file_size(download->path) == download->file.file_size && !ma_sha256_file(download->path, hash) &&
        !memcmp(hash, download->file.content_hash, 32);
    if (download->local && !download->indexed)
        download->local = !file_io_sync_file(download->path) && !file_io_sync_directory(download->path);
}
static int complete_write(sqlite3* db, void* data) {
    struct group_download* d = data;
    int rc = access_reference(db, data);
    if (!rc && d->local && !d->indexed)
        rc = media_index_commit_write(db, &d->file, d->self, NULL, d->group, d->path, d->base);
    return rc;
}
static void completed(void* data, const struct db_async_result* result) {
    struct group_download* d = data;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "group file completion ch=%s status=%d rc=%d", d->group, result->status, result->sqlite_code);
    finished(d, result->status ? -1 : 0);
}
static void transferred(void* data, int error) {
    struct group_download* d = data; d->network = 0;
    if (error || !current(d)) { finished(d, error ? error : -1); return; }
    int write = d->local && !d->indexed;
    if (db_async_can_write(d->inst->db_worker)) {
        db_async_run(d->inst->db_worker, &(struct db_async_task){.name="attachment-download-complete", .mode=write ? DB_ASYNC_WRITE : DB_ASYNC_READ,
            .guard=current, .work=complete_write, .done=completed, .data=d}); return;
    }
    sqlite3* db = CC(d->inst)->db;
    int rc = access_reference(db, d);
    if (!rc && write && media_index_commit(db, &d->file, d->self, NULL, d->group, d->path, d->base)) rc = SQLITE_ERROR;
    completed(d, &(struct db_async_result){.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc});
}
static void progress(void* data, int blocks, int total) {
    struct group_download* d = data;
    if (d->progress && current(d)) d->progress(d->arg, blocks, total);
}
static void checked(void* data, int error) {
    struct group_download* download = data;
    if (error || !current(download)) { finished(download, -1); return; }
    if (download->local) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "group attachment verified local copy ch=%s indexed=%d path=%s",
            download->group, download->indexed, download->path);
        transferred(download, 0); return;
    }
    download->network = 1;
    if (media_download_start(download->inst, topo_groups_channel_slot(download->inst->topo_groups, download->ref.space), &download->file, download->path, download->base, download->author,
        transferred, download, progress, download)) finished(download, -1);
}
static void selected(void* data, const struct db_async_result* result) {
    struct group_download* download = data;
    if (result->status || !current(download)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "group file READ completion ch=%s status=%d rc=%d: %s", download->group,
            result->status, result->sqlite_code, result->message); finished(download, -1); return;
    }
    download->path_ready = 1;
    media_async_submit(attachment_service_workers(download->inst), download->inst->ua, local_check, download, checked, download);
}
void attachment_chat_download(struct UTUN_INSTANCE* inst, const struct attachment_ref* ref,
    void (*done)(void*, int, const char*), void (*on_progress)(void*, int, int), void* arg) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group download requires done callback"); return; }
    if (!inst || !ref || !attachment_service_workers(inst) || !chat_channel_valid(ref->space) ||
        (ref->owner != ATTACHMENT_OWNER_CHAT && ref->owner != ATTACHMENT_OWNER_KANBAN)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group download invalid/stopped"); done(arg, -1, NULL); return;
    }
    struct group_download* download = u_calloc(1, sizeof(*download));
    if (!download) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group download allocation failed"); done(arg, -1, NULL); return; }
    download->ref = *ref; download->inst = inst; download->runtime = inst->attachments; memcpy(download->self, inst->my_ed25519_pubkey, sizeof(download->self));
    download->generation = chat_core_generation(inst); download->done = done; download->arg = arg; download->progress = on_progress;
    download->next = inst->attachments->downloads; inst->attachments->downloads = download;
    strcpy(download->group, ref->space);
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="attachment-download", .mode=DB_ASYNC_READ,
            .guard=current, .work=lookup, .done=selected, .data=download}); return;
    }
    sqlite3* db = CC(inst) ? CC(inst)->db : NULL;
    const char* path = db ? sqlite3_db_filename(db, "main") : NULL;
    int rc = !db || !path || *path || !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : lookup(db, download);
    selected(download, &(struct db_async_result){.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc});
}
void attachment_group_download(struct UTUN_INSTANCE* inst, const struct attachment_ref* ref,
    void (*done)(void*, int, const char*), void* arg) {
    attachment_chat_download(inst, ref, done, NULL, arg);
}

void attachment_chat_download_cancel(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32]) {
    struct attachment_service* owner = inst->attachments;
    while (owner && inst->attachments == owner) {
        struct group_download* d = owner->downloads;
        while (d && (d->cancelled || d->ref.owner != ATTACHMENT_OWNER_CHAT || strcmp(d->group, channel) || memcmp(d->ref.message, message, 32)))
            d = d->next;
        if (!d) return;
        d->cancelled = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "group file request cancelled ch=%s media=%02x%02x", channel, d->ref.object[0], d->ref.object[1]);
        if (d->network) media_download_unsubscribe(inst, topo_groups_channel_slot(inst->topo_groups, channel), d->ref.object, d);
    }
}

int attachment_group_path_busy(struct UTUN_INSTANCE* inst, const char* path) {
    for (struct group_download* d = inst->attachments ? inst->attachments->downloads : NULL; d; d = d->next)
        if (d->path_ready && !strcmp(d->path, path)) return 1;
    return media_delivery_path_busy(inst, path);
}
