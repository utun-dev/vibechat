#include "kanban_priv.h"
#include "kanban_read.h"
#include "../attachments/observe.h"

/* Принятые CREATE сохраняют доказательство публикации даже после DELETE; release управляет доступом модели. */
static int attachment_references(sqlite3* db, const char* group) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT property_id,created_by,json_object('ct',json_extract(data_json,'$.ct'),"
        "'d',json_extract(data_json,'$.d')) FROM kanban_card_properties WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND kind='message'"
        " AND json_extract(data_json,'$.ct') IS NOT NULL", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        uint8_t message[32]; const char* id = (const char*)sqlite3_column_text(st, 0);
        const char* json = (const char*)sqlite3_column_text(st, 2);
        if (!json || kb_unhex(id, message)) { rc = SQLITE_CORRUPT; break; }
        const uint8_t* ed = sqlite3_column_blob(st, 1);
        if (!ed || sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 32) { rc = SQLITE_CORRUPT; break; }
        rc = attachment_manifest_observe_write(db, ATTACHMENT_OWNER_KANBAN, group, message,
            ed, json, (size_t)sqlite3_column_bytes(st, 2), NULL);
        if (rc) break;
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (!rc) {
        rc = sqlite3_prepare_v2(db, "UPDATE attachment_refs SET released=1 WHERE owner=2 AND namespace=?1 AND released=0"
            " AND NOT EXISTS(SELECT 1 FROM kanban_card_properties p JOIN kanban_items i ON i.group_id=p.group_id AND i.id=p.card_id"
            " WHERE p.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?2) AND p.kind='message' AND p.property_id=lower(hex(attachment_refs.message)) AND p.deleted=0"
            " AND coalesce(json_extract(i.data_json,'$.deleted'),0)=0)", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_text(st, 2, group, -1, SQLITE_STATIC);
        if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
        final = sqlite3_finalize(st); if (!rc) rc = final;
    }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card attachment projection ch=%s rc=%d: %s", group, rc, sqlite3_errmsg(db));
    else DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "card attachment references staged ch=%s; commit pending", group);
    return -rc;
}

/* MEMBERS и содержимое идут независимо. Полный снимок сохраняется до получения
 * выбранных owner-блоком частей среза и всех перечисленных в них операций. */
int kb_history_complete(sqlite3* db, const char* group) {
    sqlite3_stmt *members = NULL, *st = NULL; int result = -1, rc;
    char sql[320];
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND action=9", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) goto done;
    sqlite3_int64 seals = sqlite3_column_int64(st, 0);
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
    snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey,json_extract(adm_tags,'$._kanban_seal') FROM peers_%s"
        " WHERE record_state=0 AND source=0 AND json_extract(adm_tags,'$._kanban_seal') IS NOT NULL", group);
    if (sqlite3_prepare_v2(db, sql, -1, &members, NULL) != SQLITE_OK) goto done;
    while ((rc = sqlite3_step(members)) == SQLITE_ROW) {
        const char* value = (const char*)sqlite3_column_text(members, 1); uint8_t id[32]; char head[65];
        const uint8_t* author = sqlite3_column_blob(members, 0);
        if (sqlite3_column_bytes(members, 0) != 32 || !author) goto done;
        if (kb_unhex(value, id)) goto done;
        strcpy(head, value); sqlite3_int64 remaining = seals;
        while (head[0]) {
            if (remaining-- <= 0) {
                DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "seal history awaits chain ch=%s member=%s head=%s", group,
                    sc_public_key_hex(author, (char[65]){0}), head); result = 0; goto done;
            }
            struct kb_op op = {0}; struct kb_fields f = {0};
            if (kb_unhex(head, id) || sqlite3_prepare_v2(db,
                "SELECT wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2 AND action=9", -1, &st, NULL) != SQLITE_OK ||
                sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC) != SQLITE_OK) goto done;
            int found = sqlite3_step(st);
            if (found == SQLITE_DONE) {
                DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "seal history awaits chunk ch=%s member=%s head=%s", group,
                    sc_public_key_hex(author, (char[65]){0}), head); result = 0; goto done;
            }
            if (found != SQLITE_ROW || kb_decode(db, group, sqlite3_column_blob(st, 0), sqlite3_column_bytes(st, 0), &op) ||
                kb_fields_read(db, op.body, &f)) { kb_op_free(&op); kb_fields_free(&f); goto done; }
            int valid = op.group_signed && !memcmp(f.target_ed, author, 32) && !kb_unhex(f.entity, id);
            strcpy(head, f.previous); kb_op_free(&op);
            if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; kb_fields_free(&f); goto done; } st = NULL;
            if (!valid) { kb_fields_free(&f); goto done; }
            for (size_t i = 0; i < f.parents_count; i++) {
                if (sqlite3_prepare_v2(db, "SELECT author,generation,action FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2", -1, &st, NULL) != SQLITE_OK ||
                    sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK ||
                    sqlite3_bind_blob(st, 2, f.parents[i], 32, SQLITE_STATIC) != SQLITE_OK) { kb_fields_free(&f); goto done; }
                found = sqlite3_step(st);
                if (found == SQLITE_DONE) {
                    char missing[65]; kb_hex(f.parents[i], missing);
                    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "seal history awaits operation ch=%s member=%s operation=%s", group,
                        sc_public_key_hex(author, (char[65]){0}), missing); result = 0; kb_fields_free(&f); goto done;
                }
                valid = found == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_blob(st, 0) && !memcmp(sqlite3_column_blob(st, 0), author, 32) &&
                    sqlite3_column_bytes(st, 1) == 32 && !memcmp(sqlite3_column_blob(st, 1), id, 32) && sqlite3_column_int(st, 2) != KB_SEAL;
                if (sqlite3_finalize(st) != SQLITE_OK) valid = 0; st = NULL;
                if (!valid) { kb_fields_free(&f); goto done; }
            }
            kb_fields_free(&f);
        }
    }
    if (rc == SQLITE_DONE) result = 1;
done:
    if (st && sqlite3_finalize(st) != SQLITE_OK) result = -1;
    if (members && sqlite3_finalize(members) != SQLITE_OK) result = -1;
    if (result < 0) kb_error(db, group, "invalid historical seal chain");
    return result;
}

int kb_cache_update(sqlite3* db, const char* group, const uint8_t self[32]) {
    int complete = kb_history_complete(db, group); if (complete < 0) return -1;
    char* snapshot = NULL;
    if (complete && kb_build_snapshot(db, group, self, &snapshot)) return -1;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO kanban_state(group_id,snapshot,pending) VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3)"
        " ON CONFLICT(group_id) DO UPDATE SET snapshot=CASE WHEN excluded.pending=0 THEN excluded.snapshot ELSE snapshot END,"
        " pending=excluded.pending", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, snapshot ? snapshot : "{\"columns\":[],\"cards\":[]}", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 3, !complete);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); u_free(snapshot);
    if (rc != SQLITE_DONE || final != SQLITE_OK) return kb_error(db, group, "cache coherent board");
    if (complete) {
        rc = sqlite3_prepare_v2(db, "DELETE FROM kanban_items WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id NOT IN"
            " (SELECT json_extract(j.value,'$.id') FROM kanban_state s,json_each(s.snapshot,'$.cards') j WHERE s.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)"
            " UNION ALL SELECT json_extract(j.value,'$.id') FROM kanban_state s,json_each(s.snapshot,'$.columns') j WHERE s.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1))",
            -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK) return kb_error(db, group, "remove inactive current items");
        rc = sqlite3_prepare_v2(db, "INSERT INTO kanban_items(group_id,id,kind,ordinal,data_json)"
            " SELECT s.group_id,json_extract(j.value,'$.id'),'card',j.key,j.value FROM kanban_state s,json_each(s.snapshot,'$.cards') j"
            " WHERE s.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) UNION ALL SELECT s.group_id,json_extract(j.value,'$.id'),'column',j.key,j.value"
            " FROM kanban_state s,json_each(s.snapshot,'$.columns') j WHERE s.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)"
            " ON CONFLICT(group_id,id) DO UPDATE SET ordinal=excluded.ordinal,data_json=excluded.data_json", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK) return kb_error(db, group, "write current items");
        rc = sqlite3_prepare_v2(db, "DELETE FROM kanban_card_properties WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK) return kb_error(db, group, "clear current card collections");
        rc = sqlite3_prepare_v2(db, "INSERT INTO kanban_card_properties"
            "(group_id,card_id,kind,property_id,data_json,created_by,created_at,updated_by,updated_at,revision,deleted)"
            " SELECT i.group_id,i.id,'message',json_extract(m.value,'$.id'),m.value,"
            " unhex(json_extract(m.value,'$.author')),json_extract(m.value,'$.created_at'),"
            " unhex(json_extract(m.value,'$.author')),json_extract(m.value,'$.updated_at'),"
            " unhex(json_extract(m.value,'$.revision')),json_extract(m.value,'$.deleted')"
            " FROM kanban_items i,json_each(i.data_json,'$.messages') m WHERE i.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND i.kind='card'"
            " UNION ALL SELECT i.group_id,i.id,'assignee',json_extract(a.value,'$.id'),a.value,"
            " unhex(json_extract(a.value,'$.created_by')),json_extract(a.value,'$.created_at'),"
            " unhex(json_extract(a.value,'$.updated_by')),json_extract(a.value,'$.updated_at'),"
            " unhex(json_extract(a.value,'$.revision')),json_extract(a.value,'$.deleted')"
            " FROM kanban_items i,json_each(i.data_json,'$._assignments') a WHERE i.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND i.kind='card'", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK) return kb_error(db, group, "write current card collections");
        if (attachment_references(db, group)) return -1;
        rc = sqlite3_prepare_v2(db, "UPDATE kanban_items SET data_json=json_remove(data_json,'$.messages','$._assignments')"
            " WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND kind='card'", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK) return kb_error(db, group, "separate card chat from board");
        rc = sqlite3_prepare_v2(db, "UPDATE kanban_state SET snapshot=json_set(snapshot,'$.cards',"
            " json((SELECT json_group_array(json(data_json)) FROM"
            " (SELECT data_json FROM kanban_items WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND kind='card' ORDER BY ordinal,id))))"
            " WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK) return kb_error(db, group, "retain compact coherent board");
        DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "current cards and collections staged ch=%s; commit pending", group);
    }
    if (!complete) DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "board awaits sealed history ch=%s; previous coherent snapshot retained", group);
    return 0;
}

int kanban_snapshot(struct UTUN_INSTANCE* inst, const char* group, char** result) {
    if (result) *result = NULL;
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!result || kb_type(db, group)) return kb_error(db, group, "snapshot arguments/type");
    int rc = kanban_snapshot_read(inst->topo_sqlite_db, group, result);
    if (rc == SQLITE_NOTFOUND) return kb_build_snapshot(db, group, inst->my_ed25519_pubkey, result);
    if (rc != SQLITE_OK) return kb_error(db, group, "read coherent board");
    return 0;
}
