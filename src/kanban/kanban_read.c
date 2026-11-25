#include "kanban_read.h"
#include "../chat/chat_core.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <string.h>

/* Две колонки: подтверждённый group_type и JSON. Bind/step/copy/finalize завершаются до передачи результата. */
static int read_json(sqlite3* db, const char* group, const char* entity, const char* sql, char** json) {
    if (json) *json = NULL;
    if (!db || !json || !chat_channel_valid(group)) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board READ: invalid database/group/output"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* statement = NULL; char* output = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK) { stage = "bind group"; rc = sqlite3_bind_text(statement, 1, group, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK && entity) { stage = "bind entity"; rc = sqlite3_bind_text(statement, 2, entity, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(statement); }
    if (rc == SQLITE_ROW) {
        stage = "group type"; const char* type = (const char*)sqlite3_column_text(statement, 0);
        if (!type) rc = sqlite3_errcode(db) == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_CORRUPT;
        else if (strcmp(type, "kanban")) rc = SQLITE_AUTH;
        else {
            stage = "JSON copy";
            const char* text = (const char*)sqlite3_column_text(statement, 1);
            int bytes = sqlite3_column_bytes(statement, 1);
            if (!text) rc = sqlite3_errcode(db) == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_NOTFOUND;
            else if (bytes <= 0 || memchr(text, 0, (size_t)bytes)) rc = SQLITE_CORRUPT;
            else {
                output = u_malloc((size_t)bytes + 1);
                if (!output) rc = SQLITE_NOMEM;
                else { memcpy(output, text, (size_t)bytes); output[bytes] = 0; rc = SQLITE_OK; }
            }
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(statement);
    if (final != SQLITE_OK) { stage = "finalize"; rc = final; }
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board READ failed ch=%s history=%d stage=%s rc=%d (%s) finalize=%d: %s",
            group, entity != NULL, stage, rc, sqlite3_errstr(rc), final, sqlite3_errmsg(db)); u_free(output); return rc;
    }
    *json = output;
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "board READ complete ch=%s history=%d bytes=%zu", group, entity != NULL, strlen(output));
    return SQLITE_OK;
}

/* Текущие materialized rows и pending-флаг читаются одним SQL-снимком, без сбора доски из журнала. */
int kanban_snapshot_read(sqlite3* db, const char* group, char** json) {
    return read_json(db, group, NULL,
        "SELECT c.group_type,CASE WHEN s.pending=0 THEN json_set(s.snapshot,"
        "'$.cards',json((SELECT json_group_array(json(data_json)) FROM"
        " (SELECT data_json FROM kanban_items WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND kind='card' ORDER BY ordinal,id))),"
        "'$.columns',json((SELECT json_group_array(json(data_json)) FROM"
        " (SELECT data_json FROM kanban_items WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND kind='column' ORDER BY ordinal,id))))"
        " ELSE json_set(s.snapshot,'$.pending_history',json('true'),'$.can_write',json('false'),"
        "'$.can_manage',json('false'),'$.can_moderate',json('false')) END"
        " FROM channels c LEFT JOIN kanban_state s ON s.group_id=c.ed25519_pubkey WHERE c.channel_id=?1", json);
}

/* JSON1 сохраняет типы вложенного тела/флагов и экранирует строки. Автор выдаётся полным Ed25519 в hex. */
int kanban_history_read(sqlite3* db, const char* group, const char* entity, char** json) {
    if (!entity || strlen(entity) != 64 || strspn(entity, "0123456789abcdef") != 64) {
        if (json) *json = NULL;
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board history READ: invalid entity"); return SQLITE_MISUSE;
    }
    return read_json(db, group, entity,
        "SELECT c.group_type,(SELECT json_group_array(json_object('body',json(body),'author',lower(hex(author)),"
        "'time',time,'active',json(CASE WHEN active THEN 'true' ELSE 'false' END),'id',hex(id))) FROM"
        " (SELECT body,author,time,active,id FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND entity=?2 ORDER BY time,author,id))"
        " FROM channels c WHERE c.channel_id=?1", json);
}

int kanban_messages_read(sqlite3* db, const char* group, const char* command, char** json) {
    if (json) *json = NULL;
    sqlite3_stmt* st = NULL;
    int rc = db && command ? sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.entity'),"
        "coalesce(json_extract(?1,'$.before'),''),coalesce(json_extract(?1,'$.around'),'')", -1, &st, NULL) : SQLITE_MISUSE;
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, command, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        rc = SQLITE_OK;
        for (int i = 0; i < 3; i++) {
            const char* value = (const char*)sqlite3_column_text(st, i); int bytes = sqlite3_column_bytes(st, i);
            if (!value || sqlite3_column_type(st, i) != SQLITE_TEXT || (bytes != 64 && (!i || bytes)) ||
                (size_t)bytes != strspn(value, "0123456789abcdef")) rc = SQLITE_MISMATCH;
        }
        if (sqlite3_column_bytes(st, 1) && sqlite3_column_bytes(st, 2)) rc = SQLITE_MISMATCH;
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_OK || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card messages READ: invalid query/SQL ch=%s rc=%d finalize=%d",
            group ? group : "", rc, final); return rc != SQLITE_OK ? rc : final;
    }
    return read_json(db, group, command,
        "WITH args AS (SELECT json_extract(?2,'$.entity') AS entity,coalesce(json_extract(?2,'$.before'),'') AS before,"
        "coalesce(json_extract(?2,'$.around'),'') AS around),"
        "scoped AS (SELECT p.* FROM kanban_card_properties p,args WHERE p.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND p.card_id=args.entity AND p.kind='message'),"
        "anchor AS (SELECT p.created_at,p.property_id FROM scoped p,args WHERE p.property_id=args.around)"
        " SELECT c.group_type,json_object('around',args.around,'target_found',"
        "json(CASE WHEN args.around='' OR EXISTS(SELECT 1 FROM anchor) THEN 'true' ELSE 'false' END),"
        "'messages',json((SELECT json_group_array(json(data_json)) FROM"
        " (SELECT json_set(CASE WHEN p.deleted THEN json_remove(p.data_json,'$.ct','$.d') ELSE p.data_json END,"
        "'$.file_path',coalesce((SELECT f.location FROM attachment_refs r JOIN media_files f"
        " ON f.media_id=r.object AND f.chat_id=r.namespace AND f.chunk=0 AND f.offset=0 WHERE r.owner=2"
        " AND r.namespace=lower(hex(p.group_id)) AND r.message=unhex(p.property_id) AND r.released=0 LIMIT 1),'')) AS data_json"
        " FROM scoped p WHERE (args.around='' OR EXISTS(SELECT 1 FROM anchor)) AND (args.before='' OR"
        " (p.created_at,p.property_id)<(SELECT q.created_at,q.property_id FROM scoped q WHERE q.property_id=args.before))"
        " ORDER BY p.created_at DESC,p.property_id DESC LIMIT 51 OFFSET CASE WHEN (SELECT around FROM args)='' THEN 0 ELSE"
        " max(0,(SELECT count(*) FROM scoped n,anchor a WHERE (n.created_at,n.property_id)>(a.created_at,a.property_id))-25) END))))"
        " FROM channels c,args WHERE c.channel_id=?1", json);
}
