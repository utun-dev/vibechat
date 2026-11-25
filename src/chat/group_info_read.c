#include "group_info_read.h"
#include "group_info.h"
#include "chat_core.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include "../../lib/strbuf.h"
#include <sqlite3.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

int group_info_snapshot_read(sqlite3* db, const char* channel, const uint8_t self_ed[32], char** result) {
    if (result) *result = NULL;
    if (!db || !result || !self_ed || !chat_channel_valid(channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group settings READ: invalid database/channel/output"); return SQLITE_MISUSE;
    }
    char sql[512], description[GROUP_INFO_MAX + 1] = {0}, token[65]; uint8_t hash[32];
    int length = snprintf(sql, sizeof(sql), "SELECT c.description_json,CASE WHEN c.ed25519_privkey IS NULL THEN 0"
        " WHEN typeof(c.ed25519_privkey)='blob' AND length(c.ed25519_privkey)=32 THEN 1 ELSE -1 END,"
        "EXISTS(SELECT 1 FROM peers_%s p WHERE p.ed25519_pubkey=?2 AND p.record_state=0 AND p.source=0 AND p.deleted=0 AND p.identity_verified=1 AND p.admitted=1)"
        " FROM channels c WHERE c.channel_id=?1", channel);
    if (length < 0 || (size_t)length >= sizeof(sql)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group settings READ SQL exceeds buffer ch=%s bytes=%d", channel, length); return SQLITE_TOOBIG;
    }
    sqlite3_stmt* statement = NULL; const char* stage = "prepare"; struct strbuf response = {0}; int can_edit = 0;
    int stored_type = SQLITE_NULL, stored_bytes = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK) { stage = "bind channel"; rc = sqlite3_bind_text(statement, 1, channel, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind self"; rc = sqlite3_bind_blob(statement, 2, self_ed, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(statement); }
    if (rc == SQLITE_DONE) { stage = "group missing"; rc = SQLITE_NOTFOUND; }
    else if (rc == SQLITE_ROW) {
        stage = "description fields";
        stored_type = sqlite3_column_type(statement, 0); stored_bytes = sqlite3_column_bytes(statement, 0);
        const char* value = (const char*)sqlite3_column_text(statement, 0);
        if (stored_type != SQLITE_TEXT) { stage = "description type"; rc = SQLITE_CORRUPT; }
        else if (stored_bytes < 0 || stored_bytes > GROUP_INFO_MAX) { stage = "description size"; rc = SQLITE_CORRUPT; }
        else if (!value) { stage = "description allocation"; rc = SQLITE_NOMEM; }
        else if (memchr(value, 0, (size_t)stored_bytes)) { stage = "description NUL"; rc = SQLITE_CORRUPT; }
        else {
            memcpy(description, value, (size_t)stored_bytes); description[stored_bytes] = 0;
            int key = sqlite3_column_int(statement, 1);
            if (key < 0) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group settings READ: malformed private key disables editing ch=%s", channel);
            can_edit = key == 1 && sqlite3_column_int(statement, 2); rc = SQLITE_OK;
        }
    }
    int final = sqlite3_finalize(statement); statement = NULL;
    if (final != SQLITE_OK) { stage = "lookup finalize"; rc = final; }
    if (rc != SQLITE_OK) goto done;
    struct group_info_profile profile;
    stage = "description validation"; rc = group_info_parse(db, description, &profile);
    if (rc < 0) rc = -rc;
    if (rc != SQLITE_OK) goto done;
    stage = "description hash";
    if (!SHA256((const uint8_t*)description, strlen(description), hash)) { rc = SQLITE_ERROR; goto done; }
    for (unsigned i = 0; i < sizeof(hash); i++) snprintf(token + i * 2, 3, "%02x", hash[i]);
    stage = "compact prepare";
    rc = sqlite3_prepare_v2(db, "SELECT json(?1)", -1, &statement, NULL);
    if (rc == SQLITE_OK) { stage = "compact bind"; rc = sqlite3_bind_text(statement, 1, description, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "compact step"; rc = sqlite3_step(statement); }
    if (rc == SQLITE_ROW) {
        stage = "response copy";
        const char* compact = (const char*)sqlite3_column_text(statement, 0);
        if (!compact || strbuf_addf(&response, "{\"ok\":true,\"can_edit\":%s,\"token\":\"%s\",\"description\":%s}",
            can_edit ? "true" : "false", token, compact) < 0) rc = SQLITE_NOMEM;
        else rc = SQLITE_OK;
    }
    final = sqlite3_finalize(statement); statement = NULL;
    if (final != SQLITE_OK) { stage = "compact finalize"; rc = final; }
done:
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group settings READ failed ch=%s peer=%s stage=%s type=%d bytes=%d rc=%d (%s): %s",
            channel, sc_public_key_hex(self_ed, (char[65]){0}), stage, stored_type, stored_bytes, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    } else {
        *result = strbuf_detach(&response);
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group settings READ complete ch=%s can_edit=%d bytes=%zu", channel, can_edit, strlen(*result));
    }
    strbuf_free(&response); return rc;
}
