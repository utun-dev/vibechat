#include "kanban_priv.h"
#include "kanban_read.h"
#include "../attachments/store.h"

int kanban_prepare_group_write(sqlite3* db, const char* group, const uint8_t self[32]) {
    if (!db || !chat_channel_valid(group) || !self || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board group preparation requires database/group/self and caller WRITE"); return SQLITE_MISUSE;
    }
    int rc = kb_reindex(db, group, self);
    if (rc < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board group preparation failed ch=%s self=%s rc=%d", group,
            sc_public_key_hex(self, (char[65]){0}), -rc); return -rc;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "board group snapshot and current items prepared ch=%s self=%s; commit pending",
        group, sc_public_key_hex(self, (char[65]){0})); return SQLITE_OK;
}

/* Lookup использует уже согласованный снимок доски, а не состояние GUI. */
static int lookup(sqlite3* db, const char* board, int column, const char* entity, int* deleted) {
    const char* sql = column ? "SELECT json_extract(value,'$.deleted') FROM json_each(?1,'$.columns') WHERE json_extract(value,'$.id')=?2"
        : "SELECT json_extract(value,'$.deleted') FROM json_each(?1,'$.cards') WHERE json_extract(value,'$.id')=?2";
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, board, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, entity, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW, value = found ? sqlite3_column_int(st, 0) : 0;
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "entity lookup failed entity=%s column=%d rc=%d finalize=%d: %s",
            entity, column, rc, final, sqlite3_errmsg(db)); return -(rc == SQLITE_ROW || rc == SQLITE_DONE ? final : rc);
    }
    if (found) *deleted = value;
    return found;
}

/* Отсутствие явных parents означает редактирование всех текущих голов текста. */
static int default_parents(sqlite3* db, const char* board, struct kb_fields* fields) {
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db,
        "SELECT h.value FROM json_each(?1,'$.cards') c,json_each(c.value,'$.heads') h WHERE json_extract(c.value,'$.id')=?2",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, board, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, fields->entity, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* id = (const char*)sqlite3_column_text(st, 0);
        if (fields->parents_count == KB_PARENTS_MAX || kb_unhex(id, fields->parents[fields->parents_count])) {
            rc = SQLITE_CONSTRAINT; break;
        }
        fields->parents_count++;
    }
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE && final == SQLITE_OK && fields->parents_count) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "text parents lookup failed entity=%s count=%zu rc=%d finalize=%d: %s",
        fields->entity, fields->parents_count, rc, final, sqlite3_errmsg(db));
    return -(rc != SQLITE_DONE ? rc : final != SQLITE_OK ? final : SQLITE_CONSTRAINT);
}

/* Каноническое тело подписывается по замороженной команде внутри той же WRITE. */
static int canonical(const struct kb_fields* fields, struct strbuf* json) {
    if (strbuf_addf(json, "{\"action\":\"%s\",\"entity\":\"%s\",\"column\":\"%s\",\"before\":\"%s\"",
        kb_action_name(fields->action), fields->entity, fields->column, fields->before) < 0) goto fail;
    if (*fields->color && strbuf_addf(json, ",\"color\":\"%s\"", fields->color) < 0) goto fail;
    if (*fields->grant && strbuf_addf(json, ",\"grant\":\"%s\"", fields->grant) < 0) goto fail;
    if (*fields->cutoff && strbuf_addf(json, ",\"cutoff\":\"%s\"", fields->cutoff) < 0) goto fail;
    if (fields->has_target && strbuf_addf(json, ",\"target\":\"%s\"", sc_public_key_hex(fields->target_ed, (char[65]){0})) < 0) goto fail;
    if (fields->text && (strbuf_addf(json, ",\"text\":") < 0 || strbuf_add_json_string(json, fields->text))) goto fail;
    if (fields->attachment && (strbuf_addf(json, ",\"ct\":\"%s\",\"d\":", fields->content_type) < 0 ||
        strbuf_add_json_string(json, fields->attachment))) goto fail;
    if (strbuf_addf(json, ",\"parents\":[") < 0) goto fail;
    for (size_t i = 0; i < fields->parents_count; i++) {
        char id[65]; kb_hex(fields->parents[i], id);
        if (strbuf_addf(json, "%s\"%s\"", i ? "," : "", id) < 0) goto fail;
    }
    if (strbuf_addf(json, "]}") >= 0) return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "canonical command allocation failed action=%d", fields->action); return -SQLITE_NOMEM;
}

/* Локальная команда проверяется по committed текущей карточке перед подписью. */
static int card_permission(sqlite3* db, const char* group, const struct kanban_identity* writer, struct kb_fields* f) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT json_extract(data_json,'$.author'),json_extract(data_json,'$.grant')"
        " FROM kanban_items WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2 AND kind='card'", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, f->entity, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    uint8_t owner[32]; char grant[65] = {0};
    if (rc == SQLITE_ROW) {
        const char* value = (const char*)sqlite3_column_text(st, 0);
        const char* epoch = (const char*)sqlite3_column_text(st, 1);
        uint8_t id[32];
        if (kb_unhex(value, owner) || kb_unhex(epoch, id)) rc = SQLITE_CORRUPT;
        else { strcpy(grant, epoch); rc = SQLITE_OK; }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_OK || final != SQLITE_OK) return kb_error(db, group, "card permission READ");
    uint8_t key[32]; int admin = !topo_node_sqlite_channel_get_priv(db, group, key);
    OPENSSL_cleanse(key, sizeof(key));
    int active = kb_member_access(db, group, owner, 1);
    if (active < 0) return -1;
    int privileged = !memcmp(owner, writer->public_key, 32) || (!active && admin);
    if (f->action == KB_CARD_AUTHOR || f->action == KB_CARD_DUE ||
        ((f->action == KB_ASSIGN || f->action == KB_UNASSIGN) && memcmp(f->target_ed, writer->public_key, 32))) {
        if (!privileged || (*f->grant && strcmp(f->grant, grant))) return 0;
        strcpy(f->grant, grant);
        if (memcmp(owner, writer->public_key, 32)) {
            char sql[256]; snprintf(sql, sizeof(sql), "SELECT json_extract(adm_tags,'$._kanban_seal') FROM peers_%s"
                " WHERE ed25519_pubkey=?1 AND record_state=0 AND source=0", group);
            rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
            if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, owner, 32, SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_step(st);
            const char* cutoff = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
            uint8_t id[32]; int valid = cutoff && !kb_unhex(cutoff, id);
            if (valid) strcpy(f->cutoff, cutoff);
            final = sqlite3_finalize(st);
            if (rc != SQLITE_ROW || final != SQLITE_OK || !valid) return kb_error(db, group, "inactive author cutoff READ");
            DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "admin recovery staged ch=%s card=%s author=%s cutoff=%s",
                group, f->entity, sc_public_key_hex(owner, (char[65]){0}), f->cutoff);
        } else if (*f->cutoff) return 0;
    }
    if (f->action == KB_CARD_AUTHOR || f->action == KB_ASSIGN) {
        int target = kb_member_access(db, group, f->target_ed, 1); if (target <= 0) return target;
    }
    if (f->action == KB_CARD_DELETE && memcmp(owner, writer->public_key, 32)) {
        struct kb_op permission = {.group_signed=admin,
            .body="{\"action\":\"card_delete\",\"entity\":\"0000000000000000000000000000000000000000000000000000000000000000\"}"};
        memcpy(permission.key, writer->public_key, 32);
        return kb_delete_allowed(db, group, &permission);
    }
    if (f->action == KB_MESSAGE_EDIT || f->action == KB_MESSAGE_DELETE) {
        rc = sqlite3_prepare_v2(db, "SELECT created_by,deleted FROM kanban_card_properties"
            " WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND card_id=?2 AND kind='message' AND property_id=?3", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, f->entity, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, f->before, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int allowed = rc == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_blob(st, 0) && !memcmp(sqlite3_column_blob(st, 0), writer->public_key, 32) && !sqlite3_column_int(st, 1);
        final = sqlite3_finalize(st);
        if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) return kb_error(db, group, "message permission READ");
        return allowed;
    }
    return 1;
}

int kanban_command_write(sqlite3* db, void* data) {
    struct kanban_write* query = data;
    if (!query) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "kanban WRITE missing query"); return SQLITE_MISUSE; }
    query->result = NULL; query->error = "invalid_command";
    if (!db || !chat_channel_valid(query->group) || !query->json || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "kanban WRITE requires database/group/command/author and caller transaction");
        return SQLITE_MISUSE;
    }
    const char* group = query->group; const struct kanban_identity* writer = &query->writer;
    struct kb_fields fields = {0}; struct kb_op op = {0}; struct strbuf body = {0}, response = {0}; char* board = NULL;
    const char* stage = "type"; int rc = kb_type(db, group);
    if (rc < 0) { rc = -rc; goto done; }
    stage = "command body";
    if (kb_fields_read(db, query->json, &fields) || fields.action == KB_SEAL) { rc = SQLITE_CONSTRAINT; goto done; }
    stage = "current author";
    enum ms_author_status author = member_sync_verify_author(db, group, writer->public_key, &rc);
    if (author == MS_AUTHOR_ERROR) goto done;
    if (author != MS_AUTHOR_ACTIVE) {
        query->error = "forbidden"; rc = SQLITE_AUTH; goto done;
    }
    stage = "current board"; rc = kanban_snapshot_read(db, group, &board);
    if (rc == SQLITE_NOTFOUND) { rc = kb_build_snapshot(db, group, writer->public_key, &board); if (rc < 0) rc = -rc; }
    if (rc != SQLITE_OK) goto done;
    static const uint8_t empty[16]; int attachment = memcmp(query->attachment_job, empty, 16) != 0;
    if (attachment) {
        enum attachment_job_state state; uint8_t message[32];
        stage = "attachment intent"; rc = attachment_store_job_read(db, query->attachment_job, &state, message); if (rc) goto done;
        if (fields.action != KB_MESSAGE_CREATE || !fields.attachment) { rc = SQLITE_CONSTRAINT; goto done; }
        if (state == ATTACHMENT_JOB_PUBLISHED) {
            uint8_t token[32]; rc = group_policy_key_generation(db, group, writer->public_key, token);
            if (rc) { rc = SQLITE_AUTH; goto done; }
            rc = attachment_store_authorize_read(db, query->attachment_job, ATTACHMENT_OWNER_KANBAN,
                group, writer->public_key, token); if (rc) goto done;
            struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_KANBAN}; strcpy(ref.space, group);
            memcpy(ref.message, message, 32); memcpy(ref.object, query->attachment_object, 16);
            rc = attachment_store_publish_write(db, query->attachment_job, &ref); if (rc) goto done;
            char id[65]; kb_hex(message, id);
            if (strbuf_addf(&response, "{\"ok\":true,\"id\":\"%s\",\"board\":%s}", id, board) < 0) { rc = SQLITE_NOMEM; goto done; }
            query->result = strbuf_detach(&response); query->error = NULL;
            DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "card attachment publication replay ch=%s id=%s", group, id); goto done;
        }
    }
    int deleting = 0, is_column = fields.action >= KB_COLUMN_CREATE && fields.action <= KB_COLUMN_DELETE; sqlite3_stmt* st = NULL;
    stage = "history state"; rc = sqlite3_prepare_v2(db, "SELECT pending FROM kanban_state WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int waiting = rc == SQLITE_ROW && sqlite3_column_int(st, 0), final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) { rc = rc == SQLITE_ROW || rc == SQLITE_DONE ? final : rc; goto done; }
    if (waiting) { query->error = "history_sync_pending"; rc = SQLITE_CONSTRAINT; goto done; }
    if (is_column) {
        uint8_t private_key[32]; int has_key = !topo_node_sqlite_channel_get_priv(db, group, private_key);
        OPENSSL_cleanse(private_key, sizeof(private_key));
        if (!has_key) { query->error = "forbidden"; rc = SQLITE_AUTH; goto done; }
    }
    if (fields.action != KB_CARD_CREATE && fields.action != KB_COLUMN_CREATE) {
        stage = "target entity"; rc = lookup(db, board, is_column, fields.entity, &deleting);
        if (rc < 0) { rc = -rc; goto done; }
        if (!rc || deleting) { query->error = "entity_deleted_or_missing"; rc = SQLITE_CONSTRAINT; goto done; }
    }
    if (fields.action == KB_CARD_CREATE || fields.action == KB_CARD_MOVE) {
        stage = "target column"; rc = lookup(db, board, 1, fields.column, &deleting);
        if (rc < 0) { rc = -rc; goto done; }
        if (!rc || deleting) { query->error = "column_deleted_or_missing"; rc = SQLITE_CONSTRAINT; goto done; }
    }
    if (fields.action == KB_COLUMN_DELETE) {
        stage = "column deletion";
        rc = sqlite3_prepare_v2(db, "SELECT"
            " (SELECT count(*) FROM json_each(?1,'$.columns') WHERE NOT json_extract(value,'$.deleted')),"
            " (SELECT count(*) FROM json_each(?1,'$.cards') WHERE NOT json_extract(value,'$.deleted') AND json_extract(value,'$.column')=?2)",
            -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, board, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, fields.entity, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int allowed = rc == SQLITE_ROW && sqlite3_column_int(st, 0) > 1 && !sqlite3_column_int(st, 1); final = sqlite3_finalize(st);
        if (rc != SQLITE_ROW || final != SQLITE_OK) { rc = rc == SQLITE_ROW ? final : rc; goto done; }
        if (!allowed) { query->error = "column_not_empty_or_last"; rc = SQLITE_CONSTRAINT; goto done; }
    }
    if (fields.action == KB_CARD_EDIT && !fields.parents_count) {
        stage = "default text parents"; rc = default_parents(db, board, &fields);
        if (rc < 0) { query->error = "text_dependencies"; rc = -rc; goto done; }
    }
    if (fields.action >= KB_CARD_AUTHOR || fields.action == KB_CARD_DELETE) {
        stage = "card permission"; int allowed = card_permission(db, group, writer, &fields);
        if (allowed <= 0) { query->error = "forbidden"; rc = allowed < 0 ? SQLITE_ERROR : SQLITE_AUTH; goto done; }
    }
    if (fields.action == KB_CARD_EDIT) for (size_t i = 0; i < fields.parents_count; i++) {
        stage = "text dependency";
        rc = sqlite3_prepare_v2(db, "SELECT 1 FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2 AND entity=?3 AND action IN(1,2) AND active=1",
            -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, fields.parents[i], 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, fields.entity, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        final = sqlite3_finalize(st);
        if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) { rc = rc == SQLITE_ROW || rc == SQLITE_DONE ? final : rc; goto done; }
        if (rc == SQLITE_DONE) { query->error = "text_dependencies"; rc = SQLITE_CONSTRAINT; goto done; }
    }
    uint8_t generation[32], key[32]; int group_sign = is_column;
    if (fields.action == KB_CARD_DELETE && !topo_node_sqlite_channel_get_priv(db, group, key)) group_sign = 1;
    if (fields.action >= KB_CARD_AUTHOR && fields.action <= KB_UNASSIGN &&
        !topo_node_sqlite_channel_get_priv(db, group, key)) group_sign = 1;
    OPENSSL_cleanse(key, sizeof(key));
    stage = "author generation"; rc = group_policy_key_generation(db, group, writer->public_key, generation);
    if (rc) { rc = -kb_error(db, group, stage); goto done; }
    if (attachment) {
        stage = "attachment author namespace"; rc = attachment_store_authorize_read(db, query->attachment_job,
            ATTACHMENT_OWNER_KANBAN, group, writer->public_key, generation); if (rc) goto done;
    }
    stage = "canonical command"; rc = canonical(&fields, &body); if (rc < 0) { rc = -rc; goto done; }
    uint8_t wire[KB_WIRE_MAX]; size_t size;
    stage = "sign operation"; rc = kb_sign(db, writer, group, strbuf_str(&body), generation, group_sign, wire, &size);
    if (rc < 0) { rc = -rc; goto done; }
    stage = "decode signed operation"; rc = kb_decode(db, group, wire, size, &op); if (rc) { rc = -rc; goto done; }
    stage = "operation authorization"; rc = kb_authorized(db, group, &op);
    if (rc < 0) { rc = -kb_error(db, group, stage); goto done; }
    if (rc != 1) { query->error = "forbidden"; rc = SQLITE_AUTH; goto done; }
    stage = "store operation"; rc = kb_store(db, group, &op); if (rc < 0) { rc = -rc; goto done; }
    stage = "current state and Merkle"; rc = kb_reindex(db, group, writer->public_key); if (rc < 0) { rc = -rc; goto done; }
    if (attachment) {
        struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_KANBAN}; strcpy(ref.space, group);
        memcpy(ref.message, op.id, 32); memcpy(ref.object, query->attachment_object, 16);
        stage = "attachment publication"; rc = attachment_store_publish_write(db, query->attachment_job, &ref); if (rc) goto done;
    }
    u_free(board); board = NULL; stage = "result board"; rc = kanban_snapshot_read(db, group, &board); if (rc) goto done;
    char id[65]; kb_hex(op.id, id); stage = "result allocation";
    if (strbuf_addf(&response, "{\"ok\":true,\"id\":\"%s\",\"board\":%s}", id, board) < 0) { rc = SQLITE_NOMEM; goto done; }
    query->result = strbuf_detach(&response); query->error = NULL; rc = SQLITE_OK;
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN,
        "command staged ch=%s card=%s action=%s signer=%s target=%s grant=%s cutoff=%s id=%s; commit pending",
        group, fields.entity, kb_action_name(fields.action), sc_public_key_hex(writer->public_key, (char[65]){0}),
        sc_public_key_hex(fields.has_target ? fields.target_ed : NULL, (char[65]){0}), fields.grant, fields.cutoff, id);
done:
    if (rc != SQLITE_OK) DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "command WRITE rejected ch=%s action=%d stage=%s reason=%s rc=%d (%s): %s",
        group, fields.action, stage, query->error, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    kb_op_free(&op); kb_fields_free(&fields); strbuf_free(&body); strbuf_free(&response); u_free(board); return rc;
}
