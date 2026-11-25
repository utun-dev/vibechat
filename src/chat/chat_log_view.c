/* Производные строки и задания: вход только из проверенного журнала, без транспорта/подписания. */
#include "chat_log_priv.h"
#include "../attachments/observe.h"
#include "../db_schema.h"
#include <limits.h>

int cl_message_syntax(sqlite3* db, const struct db_sync_record* r) {
    if (memchr(r->data, 0, r->size)) return 0;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT CASE WHEN NOT json_valid(?1) THEN 0 ELSE json_type(?1)='object'"
        " AND json_type(?1,'$.ct')='text' AND json_type(?1,'$.d')='text'"
        " AND (json_type(?1,'$.reply_quote') IS NULL OR (json_type(?1,'$.reply_quote')='text'"
        " AND length(json_extract(?1,'$.reply_quote'))>0 AND instr(json_extract(?1,'$.reply_quote'),char(0))=0"
        " AND json_type(?1,'$.reply_to')='text' AND length(CAST(json_extract(?1,'$.reply_to') AS BLOB))=64"
        " AND json_extract(?1,'$.reply_to') NOT GLOB '*[^0-9a-f]*' AND json_extract(?1,'$.reply_to')!=printf('%064d',0)))"
        " AND (NOT ?2 OR (json_extract(?1,'$.deleted')=1 AND json_extract(?1,'$.ct')='text/plain'"
        " AND json_extract(?1,'$.d')='')) END", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, r->data, (int)r->size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 2, r->action == CL_DELETE);
    if (!rc) rc = sqlite3_step(st);
    int valid = rc == SQLITE_ROW && sqlite3_column_int(st, 0), final = sqlite3_finalize(st);
    return rc == SQLITE_ROW && !final ? valid : cl_error(db, "action JSON syntax", rc == SQLITE_ROW ? final : rc);
}

/* Проверяем применимость к уже выбранной версии после проверки синтаксиса. */
static int message_applicable(sqlite3* db, const struct db_sync_record* r, const char* previous, int deleted) {
    if (r->action == CL_CREATE) return previous == NULL;
    if (!previous || deleted) return 0;
    sqlite3_stmt* st = NULL;
    const char* query = r->action == CL_EDIT
        ? "SELECT json_extract(?2,'$.ct') IN('text','text/plain') AND json_remove(?1,'$.d')=json_remove(?2,'$.d')"
        : "SELECT json_remove(?1,'$.ct','$.d','$.deleted')=json_remove(?2,'$.ct','$.d','$.deleted')";
    int rc = sqlite3_prepare_v2(db, query, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, r->data, (int)r->size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 2, previous, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int valid = rc == SQLITE_ROW && sqlite3_column_int(st, 0), final = sqlite3_finalize(st);
    return rc == SQLITE_ROW && !final ? valid : cl_error(db, "action permissions", rc == SQLITE_ROW ? final : rc);
}

/* Проекция может пропускать блок без остановки проверенной цепочки. */
static void log_skipped_action(const struct member_log_record* block, const char* reason) {
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "CHAT projection skipped ch=%s author=%s seq=%llu action=%u reason=%s",
        block->channel, sc_public_key_hex(block->member, (char[65]){0}), (unsigned long long)block->seq, block->body[0], reason);
}

int cl_project_scope(sqlite3* db, const char* channel, const uint8_t scope[32], int initial) {
    if (!chat_channel_valid(channel) || sqlite3_get_autocommit(db)) return cl_error(db, "projection WRITE required", SQLITE_MISUSE);
    char sql[1024];
    sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT id,ver,local_attrs,data,flags,delivered_peers,delivery_chain FROM msg_%s WHERE message_id=?", channel);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, scope, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    uint64_t local_id = 0, old_ver = 0;
    char *attrs = NULL, *old_data = NULL, *delivery = NULL;
    int flags = 0, delivered = 0;
    if (rc == SQLITE_ROW) {
        local_id = (uint64_t)sqlite3_column_int64(st, 0);
        old_ver = (uint64_t)sqlite3_column_int64(st, 1);
        attrs = u_strdup((const char*)sqlite3_column_text(st, 2));
        old_data = u_strdup((const char*)sqlite3_column_text(st, 3));
        flags = sqlite3_column_int(st, 4);
        delivered = sqlite3_column_int(st, 5);
        delivery = u_strdup((const char*)sqlite3_column_text(st, 6));
        if (!attrs || !old_data || !delivery) rc = SQLITE_NOMEM;
    }
    int final = sqlite3_finalize(st);
    st = NULL;
    int code = (rc == SQLITE_ROW || rc == SQLITE_DONE) && !final ? 0 : cl_error(db, "old projection", rc == SQLITE_ROW ? final : rc);
    if (code) goto done;
    snprintf(sql, sizeof(sql), "DELETE FROM feedback_%s WHERE message_id=?", channel);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, scope, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    code = cl_finish(db, st, rc, "clear feedback projection");
    st = NULL;
    if (code) goto done;
    rc = sqlite3_prepare_v2(db, "SELECT member_id,seq FROM member_log WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND scope_id=? ORDER BY member_id,seq", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, scope, 32, SQLITE_STATIC);
    struct db_sync_record winner = {0};
    char winner_data[DB_SYNC_MESSAGE_MAX + 1];
    int created = 0, deleted = 0;
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const uint8_t* member = sqlite3_column_blob(st, 0);
            if (!member || sqlite3_column_bytes(st, 0) != 32) { rc = SQLITE_CORRUPT; break; }
            uint64_t seq = (uint64_t)sqlite3_column_int64(st, 1), local_id;
            uint8_t wire[MEMBER_LOG_WIRE_MAX];
            size_t size = sizeof(wire);
            struct member_log_record b;
            if (member_log_get(db, channel, member, seq, wire, &size) != 1 ||
                member_log_decode(wire, size, member, &b)) {
                rc = SQLITE_CORRUPT;
                break;
            }
            strcpy(b.channel, channel);
            if (b.body[0] == MEMBER_LOG_INVITATION) continue;
            int allowed = cl_authorized(db, channel, member, seq, b.grant);
            if (allowed < 0) {
                rc = -allowed;
                break;
            }
            if (!allowed) continue;
            if (b.body[0] == CL_FEEDBACK) {
                struct chat_feedback_record feedback;
                if (cl_decode_feedback(&b, &feedback)) {
                    log_skipped_action(&b, "feedback format");
                    continue;
                }
                int wrote = chat_feedback_project_write(db, channel, &feedback);
                if (wrote < 0) {
                    rc = -wrote;
                    break;
                }
            } else {
                struct db_sync_record r;
                if (cl_decode_message(&b, &r)) {
                    log_skipped_action(&b, "message fields");
                    continue;
                }
                int valid = cl_message_syntax(db, &r);
                if (valid > 0) valid = message_applicable(db, &r, created ? winner_data : NULL, deleted);
                if (valid < 0) {
                    rc = -valid;
                    break;
                }
                if (!valid) {
                    log_skipped_action(&b, "message syntax or current state");
                    continue;
                }
                int lookup = topo_node_sqlite_member_local_id(db, channel, member, 0, &local_id, &rc);
                if (lookup) { if (!rc) rc = SQLITE_AUTH; break; }
                rc = SQLITE_ROW;
                r.author = local_id;
                if (r.action == CL_CREATE) {
                    enum attachment_observe_skip skip;
                    int observed = attachment_manifest_observe_write(db, ATTACHMENT_OWNER_CHAT, channel, scope,
                        r.author_ed, r.data, r.size, &skip);
                    if (observed) { rc = observed; break; }
                    if (skip == ATTACHMENT_OBSERVE_INVALID) { log_skipped_action(&b, "attachment manifest"); continue; }
                    if (skip == ATTACHMENT_OBSERVE_CONFLICT)
                        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "CHAT attachment skipped ch=%s author=%s seq=%llu action=%u reason=conflicting UUID",
                            channel, sc_public_key_hex(member, (char[65]){0}), (unsigned long long)seq, b.body[0]);
                }
                winner = r;
                memcpy(winner_data, r.data, r.size);
                winner_data[r.size] = 0;
                winner.data = winner_data;
                created = 1;
                deleted = r.action == CL_DELETE;
            }
        }
    code = cl_finish(db, st, rc, "replay object");
    st = NULL;
    if (code) goto done;
    rc = sqlite3_prepare_v2(db, "UPDATE attachment_refs SET released=?3 WHERE owner=1 AND namespace=?1 AND message=?2", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, scope, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 3, !created || deleted);
    if (!rc) rc = sqlite3_step(st);
    code = cl_finish(db, st, rc, "attachment reference visibility");
    st = NULL;
    if (code) goto done;
    if (created) {
        if (!local_id) {
            snprintf(sql, sizeof(sql), "SELECT COALESCE(max(id),0)+1 FROM msg_%s", channel);
            rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
            if (!rc) rc = sqlite3_step(st);
            local_id = rc == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
            final = sqlite3_finalize(st);
            st = NULL;
            if (!local_id || local_id > INT64_MAX || final) {
                code = cl_error(db, "local message ID", SQLITE_FULL);
                goto done;
            }
        }
        struct db_sync_scope projection = {0}; strcpy(projection.channel, channel);
        snprintf(projection.table, sizeof(projection.table), "msg_%s", channel);
        winner.local_id = local_id;
        winner.local_attrs = attrs;
        code = db_sync_project_write(db, &projection, &winner);
        if (code) {
            code = -code;
            goto done;
        }
        snprintf(sql, sizeof(sql), "UPDATE msg_%s SET flags=?2,delivered_peers=?3,delivery_chain=?4 WHERE message_id=?1", channel);
        rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, scope, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int(st, 2, old_ver == winner.revision.ver ? flags : 0);
        if (!rc) rc = sqlite3_bind_int(st, 3, old_ver == winner.revision.ver ? delivered : 0);
        if (!rc) rc = sqlite3_bind_text(st, 4, old_ver == winner.revision.ver && delivery ? delivery : "", -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        code = cl_finish(db, st, rc, "local message metadata");
        st = NULL;
        if (code) goto done;
    } else {
        snprintf(sql, sizeof(sql), "DELETE FROM msg_%s WHERE message_id=?", channel);
        rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, scope, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        code = cl_finish(db, st, rc, "remove inadmissible message");
        st = NULL;
        if (code) goto done;
    }
    if (created && old_ver != winner.revision.ver) {
        rc = sqlite3_prepare_v2(
            db,
            "INSERT INTO chat_effects(group_id,message_id,version,initial,updated) VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3,?4,?5)"
            " ON CONFLICT(group_id,message_id) DO UPDATE SET version=excluded.version,initial=excluded.initial,updated=excluded.updated,notified=0",
            -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, scope, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)winner.revision.ver);
        if (!rc) rc = sqlite3_bind_int(st, 4, initial);
        if (!rc) rc = sqlite3_bind_int(st, 5, old_ver != 0);
        if (!rc) rc = sqlite3_step(st);
        code = cl_finish(db, st, rc, "schedule content effects");
        st = NULL;
    }
    if (!created) {
        rc = sqlite3_prepare_v2(db, "DELETE FROM chat_effects WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND message_id=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, scope, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        code = cl_finish(db, st, rc, "cancel obsolete content effects");
        st = NULL;
    }
done:
    if (st) sqlite3_finalize(st);
    u_free(attrs);
    u_free(old_data);
    u_free(delivery);
    return code;
}
int chat_log_rebuild_changed(sqlite3* db, const char* channel, int* changed) {
    if (changed) *changed = 0;
    char table[96];
    snprintf(table, sizeof(table), "msg_%s", channel);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int exists = rc == SQLITE_ROW, final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final) return cl_error(db, "projection schema", rc);
    if (!exists) return 0;
    uint8_t(*ids)[32] = NULL;
    size_t count = 0;
    rc = sqlite3_prepare_v2(db, "SELECT DISTINCT scope_id FROM member_log WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND substr(body,1,1)<>x'05'", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            void* grown = count < UINT32_MAX / 32 - 1 ? u_realloc(ids, (count + 1) * 32) : NULL;
            if (!grown) {
                rc = SQLITE_NOMEM;
                break;
            }
            ids = grown;
            memcpy(ids[count++], sqlite3_column_blob(st, 0), 32);
        }
    int code = cl_finish(db, st, rc, "projection scope catalog");
    for (size_t i = 0; !code && i < count; i++)
        code = cl_project_scope(db, channel, ids[i], 1);
    u_free(ids);
    if (!code && changed) *changed = count != 0;
    return code;
}
int chat_log_rebuild(sqlite3* db, const char* channel) {
    return chat_log_rebuild_changed(db, channel, NULL);
}
int chat_log_effect_current(sqlite3* db, const char* channel, const uint8_t message[32], uint64_t version) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM msg_%s WHERE message_id=? AND ver=?", channel);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, message, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)version);
    if (!rc) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW, final = sqlite3_finalize(st);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) && !final ? found : cl_error(db, "effect generation", rc);
}
int chat_log_effect_complete(sqlite3* db, const char* channel, const uint8_t message[32], uint64_t version) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "DELETE FROM chat_effects WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND message_id=? AND version=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, message, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)version);
    if (!rc) rc = sqlite3_step(st);
    return -cl_finish(db, st, rc, "complete content effect");
}
