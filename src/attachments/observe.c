#include "observe.h"
#include "../chat/chat_core.h"
#include "manifest.h"
#include "../media_async/encrypted_file.h"
#include "../../lib/debug_config.h"
#include <string.h>

int attachment_manifest_observe_write(sqlite3* db, enum attachment_owner owner, const char* space,
    const uint8_t message[32], const uint8_t author[32], const char* json, size_t length, enum attachment_observe_skip* skip) {
    if (skip) *skip = ATTACHMENT_OBSERVE_NONE;
    if (!db || sqlite3_get_autocommit(db) || (owner != ATTACHMENT_OWNER_CHAT && owner != ATTACHMENT_OWNER_KANBAN) ||
        !chat_channel_valid(space) || !message || !author || !json || !length || length > ATTACHMENT_MANIFEST_BODY_MAX || memchr(json, 0, length)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment manifest observation: invalid owner/namespace/body or missing WRITE"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; struct media_index_result parsed = {0};
    enum attachment_observe_skip reason = ATTACHMENT_OBSERVE_NONE;
    int rc = sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.ct'),json_extract(?1,'$.d'),"
        "json_object('ct',json_extract(?1,'$.ct'),'d',json_extract(?1,'$.d'))", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, json, (int)length, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* type = (const char*)sqlite3_column_text(st, 0);
        const char* body = (const char*)sqlite3_column_text(st, 1);
        const char* manifest = (const char*)sqlite3_column_text(st, 2);
        if (sqlite3_errcode(db) == SQLITE_NOMEM) rc = SQLITE_NOMEM;
        else if (!type || (strcmp(type, "application/octet-stream") && strcmp(type, "audio/opus") &&
            strcmp(type, "video/mp4") && strcmp(type, "video/x-matroska") && strcmp(type, "image/jpeg") && strcmp(type, "image/png") && strcmp(type, ENCRYPTED_ATTACHMENT_TYPE))) rc = SQLITE_OK;
        else if (!body || !manifest || sqlite3_column_type(st, 1) != SQLITE_TEXT ||
            strlen(body) != (size_t)sqlite3_column_bytes(st, 1)) {
            reason = ATTACHMENT_OBSERVE_INVALID; rc = skip ? SQLITE_OK : SQLITE_MISMATCH;
        }
        else {
            int result = attachment_manifest_parse(body, &parsed);
            if (result == -2) rc = SQLITE_NOMEM;
            else if (result) { reason = ATTACHMENT_OBSERVE_INVALID; rc = skip ? SQLITE_OK : SQLITE_MISMATCH; }
            else {
                struct attachment_object object = {.scope=ATTACHMENT_SCOPE_GROUP,
                    .size=(uint64_t)parsed.file_size, .crypto=!strcmp(type, ENCRYPTED_ATTACHMENT_TYPE) ? ATTACHMENT_CRYPTO_GROUP : ATTACHMENT_CRYPTO_NONE,
                    .manifest=manifest, .manifest_size=(size_t)sqlite3_column_bytes(st, 2)};
                struct attachment_ref ref = {.owner=owner};
                strcpy(object.space, space);
                strcpy(ref.space, object.space);
                memcpy(object.author, author, 32);
                memcpy(object.id, parsed.media_id, 16); memcpy(object.hash, parsed.content_hash, 32);
                memcpy(ref.object, parsed.media_id, 16); memcpy(ref.message, message, 32);
                int conflict = 0;
                rc = attachment_store_observe_write(db, &object, &ref, skip ? &conflict : NULL);
                if (conflict) reason = ATTACHMENT_OBSERVE_CONFLICT;
            }
        }
    }
    int final = sqlite3_finalize(st); media_index_result_free(&parsed); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment manifest observation owner=%d space=%s rc=%d (%s) finalize=%d: %s",
        owner, space, rc, sqlite3_errstr(rc), final, sqlite3_errmsg(db));
    else if (skip) *skip = reason;
    return rc;
}
