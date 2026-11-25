#include "db_changes.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/strbuf.h"
#include "../lib/u_async.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

#define AUDIT "utun_db_changes"
#define QUEUE "utun_change_queue"
#define LOG DEBUG_CATEGORY_DB_CHANGES

struct db_changes {
    sqlite3* db;
    struct UASYNC* ua;
    sqlite3** owner;
    struct timeout_node* timer;
    struct db_changes_origin local, current;
    sqlite3_int64 batch, emitted_batch;
    int ready, pending;
};

/* Явный набор полей. @ подставляется как OLD, NEW или алиас при удалении группы. */
#define F(name) "'" #name "',CASE WHEN typeof(@." #name ")='blob' THEN '[invalid blob]' ELSE @." #name " END"
#define H(name) "'" #name "',lower(hex(@." #name "))"
#define HB(label, column) "'" label "',CASE WHEN @." #column " IS NOT NULL THEN lower(hex(@." #column ")) END"
#define J(doc, field) "json_extract(" doc ",'$." field "')"
#define VALID(doc, expr) "CASE WHEN json_valid(" doc ") THEN " expr " END"
#define MESSAGE(doc) "'content_type'," VALID(doc, J(doc, "ct")) ",'text'," \
    VALID(doc, "CASE WHEN " J(doc, "ct") " IN ('text','text/plain') THEN db_changes_preview(" J(doc, "d") ") END") \
    ",'reply_to'," VALID(doc, J(doc, "reply_to")) ",'deleted'," VALID(doc, J(doc, "deleted")) ",'payload_hash',db_changes_hash(" doc ")"
#define MSG_BODY "CAST(@.data AS TEXT)"
#define CHAT_BODY "CAST(substr(@.body,58) AS TEXT)"
#define INVITE_BODY "CAST(substr(@.body,98) AS TEXT)"

struct watched_table {
    const char* name;           /* Точное имя или префикс с 64 hex-символами группы. */
    const char* kind;
    const char* group, *key;
    const char* fields;
    int dynamic;
};

static const struct watched_table watched[] = {
    {"peers_", "member", NULL, "lower(hex(@.ed25519_pubkey))",
        F(node_id) "," H(x25519_pubkey) "," F(record_state) ",'record_source',@.source," F(deleted) "," F(admitted) "," F(identity_verified) ","
        F(connected) "," F(node_type) "," F(storage) "," F(radio_muted) "," F(local_nick) "," F(join_ts) "," F(update_ts) ","
        F(userinfo) "," F(adm_tags) "," HB("signer",signed_by) "," HB("join_signature",join_sig) ","
        HB("profile_signature",update_sig) "," HB("owner_signature",adm_tags_sig) "," HB("invitation_signature",signature), 1},
    {"member_invitations", "invitation", "@.channel", "lower(hex(@.subject))||'/'||lower(hex(@.signer))",
        H(subject) ",'signer',CASE WHEN @.signer=zeroblob(32) THEN 'group' ELSE lower(hex(@.signer)) END," F(stamp) "," H(signature), 0},
    {"channels", "group", "@.channel_id", "@.channel_id",
        "'name',@.name,'description_version'," J("@.description_json", "ver") ",'group_type',@.group_type,"
        "'clipboard_members'," J("@.description_json", "clipboard_members") ",'description_hash',db_changes_hash(@.description_json),"
        H(x25519_pubkey) "," HB("owner",owner_node_id) "," H(signature) ","
        F(avatar_version) "," HB("avatar_hash",avatar_hash) "," HB("avatar_signature",avatar_signature), 0},
    {"group_policy", "group policy", "lower(hex(@.channel_id))", "lower(hex(@.channel_id))",
        F(version) "," F(all_members) "," H(signature), 0},
    {"msg_", "chat message", NULL, "lower(hex(@.message_id))",
        H(message_id) "," H(accepted_key) "," F(timestamp) "," F(ver) "," F(edited_at) "," H(generation) "," F(flags) ","
        F(delivered_peers) "," F(delivery_chain) ",'local_status'," VALID("@.local_attrs", J("@.local_attrs", "st"))
        ",'local_attributes_hash',db_changes_hash(@.local_attrs)," F(feedback_snapshot) "," H(author_signature) "," MESSAGE(MSG_BODY), 1},
    {"archive_msg_", "archived message", NULL, "lower(hex(@.message_id))",
        H(accepted_key) "," F(timestamp) "," F(ver) "," F(edited_at) "," H(generation) "," H(author_signature) "," MESSAGE(MSG_BODY), 1},
    {"feedback_", "chat feedback", NULL, "lower(hex(@.record_key))",
        H(message_id) "," H(author_ed) "," F(ver) ",'data_hex',lower(hex(@.data)),"
        "'action',CASE @.kind WHEN 1 THEN 'reaction' WHEN 2 THEN 'poll answer' ELSE 'feedback' END," H(signature), 1},
    {"member_log", "signed chat block", "lower(hex(@.group_id))", "lower(hex(@.member_id))||'/'||@.seq",
        H(member_id) "," F(seq) "," F(time_us) "," H(scope_id) "," H(grant_id) "," H(prev_hash) "," H(block_hash) "," H(signature) "," F(applied) ","
        "'action',CASE hex(substr(@.body,1,1)) WHEN '01' THEN 'message creation' WHEN '02' THEN 'feedback' WHEN '03' THEN 'message edit'"
        " WHEN '04' THEN 'message deletion' WHEN '05' THEN 'invitation' ELSE 'unknown' END,"
        "'content',CASE WHEN substr(@.body,1,1) IN(x'01',x'03',x'04') THEN json_object(" MESSAGE(CHAT_BODY) ") END,"
        "'invitation',CASE WHEN substr(@.body,1,1)=x'05' THEN " VALID(INVITE_BODY, "json(" INVITE_BODY ")") " END,"
        "'inviter_signature',CASE WHEN substr(@.body,1,1)=x'05' THEN lower(hex(substr(@.body,34,64))) END", 0},
    {"member_faults", "chat fork proof", "lower(hex(@.group_id))", "lower(hex(@.member_id))",
        H(member_id) "," F(seq) ",'first_hash',db_changes_hash(@.first_wire),'second_hash',db_changes_hash(@.second_wire),"
        "'first_signature',lower(hex(substr(@.first_wire,-64))),'second_signature',lower(hex(substr(@.second_wire,-64)))", 0},
    {"chat_seals", "chat seal", "lower(hex(@.group_id))", "lower(hex(@.id))",
        "'member_id',lower(hex(substr(@.wire,50,32))),'grant_id',lower(hex(substr(@.wire,82,32))),"
        "'checkpoint',lower(hex(substr(@.wire,114,8))),'hash',lower(hex(substr(@.wire,122,32))),"
        "'previous',lower(hex(substr(@.wire,154,32))),'signature',lower(hex(substr(@.wire,-64)))", 0},
    {"chat_seal_anchors", "chat seal checkpoint", "lower(hex(@.group_id))", "lower(hex(@.seal_id))",
        F(seq) "," H(hash), 0},
    {"chat_seal_evidence", "chat seal evidence", "lower(hex(@.group_id))", "lower(hex(@.seal_id))||'/'||@.seq",
        F(seq) ",'member_id',lower(hex(substr(@.wire,49,32))),'hash',db_changes_hash(@.wire),"
        "'signature',lower(hex(substr(@.wire,-64)))", 0},
    {"kanban_ops", "kanban operation", "lower(hex(@.group_id))", "lower(hex(@.id))",
        H(author) "," F(time) "," H(generation) "," F(entity) "," F(active) ","
        "'action'," VALID("@.body", J("@.body", "action")) ",'details',"
        VALID("@.body", "json_remove(@.body,'$.text','$.d')") ",'text',"
        VALID("@.body", "CASE WHEN " J("@.body", "action") " IN ('message_create','message_edit','message_delete')"
            " THEN db_changes_preview(" J("@.body", "text") ") ELSE " J("@.body", "text") " END")
        ",'payload_hash',db_changes_hash(@.body),'author_signature',lower(hex(substr(@.wire,-128,64))),"
        "'group_signature',CASE WHEN substr(@.wire,-64)<>zeroblob(64) THEN lower(hex(substr(@.wire,-64))) END", 0},
};

static void hex(const uint8_t* bytes, size_t size, char* dest) {
    static const char digit[] = "0123456789abcdef";
    if (!bytes) { strcpy(dest, "-"); return; }
    for (size_t i = 0; i < size; ++i) { dest[2*i] = digit[bytes[i] >> 4]; dest[2*i+1] = digit[bytes[i] & 15]; }
    dest[2*size] = 0;
}

static struct db_changes* context(sqlite3* db) {
    return db ? sqlite3_get_clientdata(db, AUDIT) : NULL;
}

static void origin_text(struct db_changes* state, const struct db_changes_origin* origin, char out[256]) {
    static const uint8_t zero[32] = {0};
    int has_ed = origin && memcmp(origin->ed, zero, sizeof(zero));
    int remote = origin && (origin->node || has_ed) &&
        (!state || (has_ed ? memcmp(origin->ed, state->local.ed, 32) != 0 : origin->node != state->local.node));
    const struct db_changes_origin* node = remote ? origin : state ? &state->local : NULL;
    char ed[65], id[17] = "-";
    hex(node && memcmp(node->ed, zero, sizeof(zero)) ? node->ed : NULL, 32, ed);
    if (node && node->node) snprintf(id, sizeof(id), "%016llx", (unsigned long long)node->node);
    snprintf(out, 256, "source=%s source_ed=%s source_node=%s via=\"%s\"", remote ? "peer" : "local", ed, id,
        origin && origin->via ? origin->via : "local write");
}

struct db_changes_origin db_changes_source(sqlite3* db, const uint8_t* ed, uint64_t node, const char* via) {
    struct db_changes* state = context(db);
    if (!state) return (struct db_changes_origin){0};
    struct db_changes_origin previous = state->current;
    state->current = (struct db_changes_origin){.node=node, .via=via};
    if (ed) {
        memcpy(state->current.ed, ed, 32);
        if (!node && !memcmp(ed, previous.ed, 32)) state->current.node = previous.node;
    }
    return previous;
}

void db_changes_restore(sqlite3* db, struct db_changes_origin previous) {
    struct db_changes* state = context(db);
    if (state) state->current = previous;
}

void db_changes_warn(sqlite3* db, const struct db_changes_origin* source, const char* group,
    const char* record, const char* reason, const uint8_t* key, const uint8_t* signature, const char* details) {
    if (!debug_should_output(DEBUG_LEVEL_WARN, LOG)) return;
    struct db_changes* state = context(db);
    char origin[256], subject[65], sig[129];
    const struct db_changes_origin local = {.via="database verification"};
    origin_text(state, source ? source : state && state->current.via ? &state->current : &local, origin);
    hex(key, 32, subject); hex(signature, 64, sig);
    DEBUG_WARN(LOG, "%s rejected: %s group=%s %s subject=%s signature=%s%s%s",
        record, reason, group ? group : "-", origin, subject, sig, details ? " " : "", details ? details : "");
}

void db_changes_warn_hash(sqlite3* db, const struct db_changes_origin* source, const char* group, const char* record,
    const char* reason, const uint8_t* key, const uint8_t* signature, const uint8_t expected[32], const uint8_t received[32]) {
    if (!debug_should_output(DEBUG_LEVEL_WARN, LOG)) return;
    char details[160], have[65], want[65];
    hex(expected, 32, want); hex(received, 32, have);
    snprintf(details, sizeof(details), "expected=%s received=%s", want, have);
    db_changes_warn(db, source, group, record, reason, key, signature, details);
}

static void sql_failure(sqlite3* db, const char* stage, const char* table, int rc) {
    DEBUG_ERROR(LOG, "DB changes log failed stage=%s table=%s rc=%d: %s", stage, table ? table : "-", rc, sqlite3_errmsg(db));
}

static const struct watched_table* watched_table(const char* name) {
    for (size_t i = 0; i < sizeof(watched)/sizeof(*watched); ++i) {
        const struct watched_table* table = &watched[i];
        size_t prefix = strlen(table->name);
        if (!table->dynamic && !strcmp(name, table->name)) return table;
        if (table->dynamic && !strncmp(name, table->name, prefix) && strlen(name + prefix) == 64 &&
            strspn(name + prefix, "0123456789abcdef") == 64) return table;
    }
    return NULL;
}

static void expression(sqlite3_str* sql, const char* fragment, const char* alias) {
    for (; *fragment; ++fragment) {
        if (*fragment == '@') sqlite3_str_appendall(sql, alias);
        else sqlite3_str_appendchar(sql, 1, *fragment);
    }
}

static void snapshot(sqlite3_str* sql, const struct watched_table* table, const char* alias) {
    sqlite3_str_appendall(sql, "json_object(");
    expression(sql, table->fields, alias);
    sqlite3_str_appendchar(sql, 1, ')');
}

static void insert_prefix(sqlite3_str* sql, const struct watched_table* table, const char* name, const char* alias) {
    sqlite3_str_appendf(sql, "INSERT INTO " QUEUE "(batch,kind,ch,record,origin,old,new) "
        "SELECT db_changes_batch(),%Q,", table->kind);
    if (table->dynamic) sqlite3_str_appendf(sql, "%Q", name + strlen(table->name));
    else expression(sql, table->group, alias);
    sqlite3_str_appendchar(sql, 1, ',');
    expression(sql, table->key, alias);
    sqlite3_str_appendall(sql, ",db_changes_origin(),");
}

static int run_generated(sqlite3* db, sqlite3_str* sql, const char* stage, const char* name) {
    int rc = sqlite3_str_errcode(sql);
    char* statement = sqlite3_str_finish(sql);
    if (!rc) rc = statement ? sqlite3_exec(db, statement, NULL, NULL, NULL) : SQLITE_NOMEM;
    if (rc) sql_failure(db, stage, name, rc);
    sqlite3_free(statement);
    return rc;
}

void db_changes_watch(sqlite3* db, const char* name) {
    struct db_changes* state = context(db);
    const struct watched_table* table = state && state->ready && name ? watched_table(name) : NULL;
    if (!table) return;
    static const char* operations[] = {"INSERT", "UPDATE", "DELETE"};
    for (size_t i = 0; i < sizeof(operations)/sizeof(*operations); ++i) {
        sqlite3_str* sql = sqlite3_str_new(db);
        sqlite3_str_appendf(sql, "CREATE TEMP TRIGGER IF NOT EXISTS \"utun_change_%w_%d\" AFTER %s ON main.\"%w\""
            " WHEN db_changes_enabled() BEGIN ", name, (int)i, operations[i], name);
        insert_prefix(sql, table, name, i == 2 ? "old" : "new");
        if (i != 0) snapshot(sql, table, "old"); else sqlite3_str_appendall(sql, "NULL");
        sqlite3_str_appendchar(sql, 1, ',');
        if (i != 2) snapshot(sql, table, "new"); else sqlite3_str_appendall(sql, "NULL");
        sqlite3_str_appendall(sql, "; END");
        if (run_generated(db, sql, "install trigger", name)) return;
    }
}

void db_changes_before_drop(sqlite3* db, const char* name) {
    struct db_changes* state = context(db);
    const struct watched_table* table = state && state->ready && debug_should_output(DEBUG_LEVEL_INFO, LOG) ? watched_table(name) : NULL;
    if (!table) return;
    int rc = sqlite3_table_column_metadata(db, "main", name, NULL, NULL, NULL, NULL, NULL, NULL);
    if (rc == SQLITE_ERROR) return;  /* DROP IF EXISTS. */
    if (rc) { sql_failure(db, "lookup table before drop", name, rc); return; }
    sqlite3_str* sql = sqlite3_str_new(db);
    insert_prefix(sql, table, name, "r");
    snapshot(sql, table, "r");
    sqlite3_str_appendf(sql, ",NULL FROM main.\"%w\" r", name);
    run_generated(db, sql, "capture deleted table", name);
}

/* Очередь содержит только валидный UTF-8. Битые входные байты заменяем по одному. */
static size_t utf8_char(const unsigned char* p, size_t left) {
    if (*p < 0x80) return 1;
    size_t n = *p >= 0xc2 && *p <= 0xdf ? 2 : *p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
    if (!n || left < n) return 0;
    for (size_t i = 1; i < n; ++i) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] >= 0xa0) ||
        (*p == 0xf0 && p[1] < 0x90) || (*p == 0xf4 && p[1] >= 0x90)) return 0;
    return n;
}

static void preview(sqlite3_context* ctx, int argc, sqlite3_value** args) {
    (void)argc;
    if (sqlite3_value_type(args[0]) != SQLITE_TEXT) return;
    const unsigned char* p = sqlite3_value_text(args[0]);
    size_t bytes = sqlite3_value_bytes(args[0]), off = 0, len = 0;
    if (!p) { sqlite3_result_error_nomem(ctx); return; }
    char out[100*4 + 4];
    for (unsigned count = 0; off < bytes && count < 100; ++count) {
        size_t n = utf8_char(p + off, bytes - off);
        if (n) { memcpy(out + len, p + off, n); len += n; off += n; }
        else { memcpy(out + len, "\xef\xbf\xbd", 3); len += 3; ++off; }
    }
    if (off < bytes) { memcpy(out + len, "\xe2\x80\xa6", 3); len += 3; }
    sqlite3_result_text(ctx, out, (int)len, SQLITE_TRANSIENT);
}

static void payload_hash(sqlite3_context* ctx, int argc, sqlite3_value** args) {
    (void)argc;
    if (sqlite3_value_type(args[0]) == SQLITE_NULL) return;
    const void* p = sqlite3_value_blob(args[0]);
    int n = sqlite3_value_bytes(args[0]);
    uint8_t hash[32]; char encoded[65];
    if (EVP_Digest(p, (size_t)n, hash, NULL, EVP_sha256(), NULL) != 1) {
        DEBUG_ERROR(LOG, "DB changes log could not hash payload bytes=%d", n); return;
    }
    hex(hash, sizeof(hash), encoded);
    sqlite3_result_text(ctx, encoded, -1, SQLITE_TRANSIENT);
}

static void enabled(sqlite3_context* ctx, int argc, sqlite3_value** args) {
    (void)argc; (void)args;
    sqlite3_result_int(ctx, debug_should_output(DEBUG_LEVEL_INFO, LOG));
}

static void batch(sqlite3_context* ctx, int argc, sqlite3_value** args) {
    (void)argc; (void)args;
    struct db_changes* state = sqlite3_user_data(ctx);
    state->pending = 1;
    sqlite3_result_int64(ctx, state->batch);
}

static void sql_origin(sqlite3_context* ctx, int argc, sqlite3_value** args) {
    (void)argc; (void)args;
    struct db_changes* state = sqlite3_user_data(ctx);
    char text[256]; origin_text(state, &state->current, text);
    sqlite3_result_text(ctx, text, -1, SQLITE_TRANSIENT);
}

/* Hook не читает SQLite и не считает попытку COMMIT подтверждением записи.
 * Номер разделяет транзакции, успевшие завершиться до одного запуска flush. */
static int next_batch(void* data) {
    struct db_changes* state = data;
    ++state->batch;
    if (state->pending && state->timer && !uasync_timer_armed(state->timer)) uasync_timer_set(state->timer, 1);
    return 0;
}

static const char* column(sqlite3_stmt* st, int index) {
    const char* text = (const char*)sqlite3_column_text(st, index);
    return text ? text : "";
}

static const char* action(sqlite3_stmt* st) {
    const char* kind = column(st, 2);
    const char* specific = column(st, 8);
    int insert = sqlite3_column_type(st, 6) == SQLITE_NULL;
    int remove = sqlite3_column_type(st, 7) == SQLITE_NULL;
    if (!strcmp(kind, "member")) {
        if (insert) return sqlite3_column_int(st, 9) ? "Member discovered" : "Member added";
        if (remove) return "Member removed";
        if (sqlite3_column_int(st, 10)) return "Member access changed";
        if (sqlite3_column_int(st, 11)) return "Member profile updated";
        if (sqlite3_column_int(st, 12)) return "Member connection state changed";
        return "Member record updated";
    }
    if (!strcmp(kind, "invitation")) return insert ? "Invitation added" : remove ? "Invitation removed" : "Invitation updated";
    if (!strcmp(kind, "chat message")) {
        if (insert) return "Chat message added to history";
        if (remove) return sqlite3_column_int(st, 16) ? "Chat message moved out of active history" : "Chat message removed from history";
        if (sqlite3_column_int(st, 13)) return "Chat message edited in history";
        if (sqlite3_column_int(st, 14)) return "Chat message delivery confirmed";
        return "Chat message state changed";
    }
    if (!strcmp(kind, "archived message")) return insert ? "Chat message archived" : remove ? "Archived message removed" : "Archived message updated";
    if (!strcmp(kind, "chat feedback")) return insert ? "Chat feedback added" : remove ? "Chat feedback removed" : "Chat feedback updated";
    if (!strcmp(kind, "kanban operation")) {
        if (remove) return "Kanban operation removed";
        if (!insert) return sqlite3_column_int(st, 15) ? "Kanban operation activated" : "Kanban operation deactivated";
        static const struct { const char* id, *action; } actions[] = {
            {"card_create","Kanban card created"}, {"card_edit","Kanban card edited"}, {"card_move","Kanban card moved"},
            {"card_delete","Kanban card deleted"}, {"column_create","Kanban column created"}, {"column_rename","Kanban column renamed"},
            {"column_move","Kanban column moved"}, {"column_delete","Kanban column deleted"}, {"seal","Kanban history sealed"},
            {"card_author","Kanban card author changed"}, {"card_due","Kanban due date changed"}, {"assign","Kanban assignee added"},
            {"unassign","Kanban assignee removed"}, {"message_create","Kanban message added"}, {"message_edit","Kanban message edited"},
            {"message_delete","Kanban message deleted"}
        };
        for (size_t i = 0; i < sizeof(actions)/sizeof(*actions); ++i) if (!strcmp(specific, actions[i].id)) return actions[i].action;
        return "Kanban operation saved";
    }
    if (!strcmp(kind, "signed chat block")) {
        if (remove) return "Signed chat block removed";
        if (!insert) return "Signed chat block updated";
        if (!strcmp(specific, "invitation")) return "Invitation receipt saved";
        return "Signed chat block saved";
    }
    if (!strcmp(kind, "chat fork proof")) return insert ? "Chat fork proof saved" : remove ? "Chat fork proof removed" : "Chat fork proof updated";
    if (!strcmp(kind, "chat seal")) return insert ? "Chat seal saved" : remove ? "Chat seal removed" : "Chat seal updated";
    if (!strcmp(kind, "chat seal checkpoint"))
        return insert ? "Chat seal checkpoint confirmed" : remove ? "Chat seal checkpoint removed" : "Chat seal checkpoint updated";
    if (!strcmp(kind, "chat seal evidence")) return insert ? "Chat seal evidence saved" : remove ? "Chat seal evidence removed" : "Chat seal evidence updated";
    if (!strcmp(kind, "group policy")) return insert ? "Group policy saved" : remove ? "Group policy removed" : "Group policy updated";
    return insert ? "Group added" : remove ? "Group removed" : "Group record updated";
}

/* Сигнатуры сохраняются целиком, остальные изменённые поля показываются парами.
 * Перенос длинных значений сохраняет UTF-8 и помещается в лимит общего logger. */
static void field_lines(const char* change, const char* key, const char* value, const char* label) {
    size_t left = strlen(value), offset = 0;
    while (left) {
        size_t n = left > 1600 ? 1600 : left;
        while (n && n < left && ((unsigned char)value[offset+n] & 0xc0) == 0x80) --n;
        if (!n) n = 1600;
        DEBUG_INFO(LOG, "change=%s %s%s%s part_offset=%zu %.*s", change, key, label ? " " : "", label ? label : "", offset, (int)n, value + offset);
        offset += n; left -= n;
    }
}

static int emit_details(sqlite3_stmt* fields, sqlite3_stmt* event, const char* change, struct strbuf* line) {
    const char* before = (const char*)sqlite3_column_text(event, 6);
    const char* after = (const char*)sqlite3_column_text(event, 7);
    int rc = sqlite3_bind_text(fields, 1, before, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(fields, 2, after, -1, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(fields)) == SQLITE_ROW) {
        const char* key = column(fields, 0), *old = column(fields, 1), *new = column(fields, 2);
        int different = sqlite3_column_int(fields, 3);
        const char* from = before && after && different ? old : "";
        const char* value = after ? new : old;
        size_t length = strlen(key) + strlen(from) + strlen(value) + 5;
        if (line->len + length > 1400 && line->len) {
            DEBUG_INFO(LOG, "%s", strbuf_str(line)); strbuf_reset(line);
        }
        if (length > 1400) {
            if (*from) field_lines(change, key, from, "before");
            field_lines(change, key, value, before && after && different ? "after" : NULL);
        } else if ((!line->len && strbuf_addf(line, "change=%s", change) < 0) ||
                   strbuf_addf(line, " %s=%s%s%s", key, from, *from ? "->" : "", value) < 0) {
            rc = SQLITE_NOMEM; break;
        }
    }
    if (line->len) { DEBUG_INFO(LOG, "%s", strbuf_str(line)); strbuf_reset(line); }
    sqlite3_reset(fields); sqlite3_clear_bindings(fields);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static int emit(struct db_changes* state, sqlite3_int64 batch) {
    static const char query[] =
        "WITH groups AS (SELECT min(id) first,max(id) last FROM " AUDIT "." QUEUE
        " WHERE batch>?1 AND batch<=?2 GROUP BY batch,kind,ch,record)"
        " SELECT a.batch,a.id,a.kind,a.ch,a.record,CASE WHEN z.new IS NULL THEN a.origin ELSE z.origin END,a.old,z.new,"
        " json_extract(coalesce(z.new,a.old),'$.action'),json_extract(z.new,'$.record_source'),"
        " json_extract(a.old,'$.admitted') IS NOT json_extract(z.new,'$.admitted')"
        " OR json_extract(a.old,'$.deleted') IS NOT json_extract(z.new,'$.deleted'),"
        " json_extract(a.old,'$.userinfo') IS NOT json_extract(z.new,'$.userinfo')"
        " OR json_extract(a.old,'$.update_ts') IS NOT json_extract(z.new,'$.update_ts')"
        " OR json_extract(a.old,'$.profile_signature') IS NOT json_extract(z.new,'$.profile_signature'),"
        " json_extract(a.old,'$.connected') IS NOT json_extract(z.new,'$.connected'),"
        " json_extract(a.old,'$.payload_hash') IS NOT json_extract(z.new,'$.payload_hash')"
        " OR json_extract(a.old,'$.ver') IS NOT json_extract(z.new,'$.ver'),"
        " json_extract(a.old,'$.delivered_peers') < json_extract(z.new,'$.delivered_peers'),json_extract(z.new,'$.active'),"
        " a.kind='chat message' AND z.new IS NULL AND EXISTS(SELECT 1 FROM " AUDIT "." QUEUE " x"
        " WHERE x.batch=a.batch AND x.kind='archived message' AND x.ch=a.ch AND x.record=a.record AND x.new IS NOT NULL)"
        " FROM groups g JOIN " AUDIT "." QUEUE " a ON a.id=g.first JOIN " AUDIT "." QUEUE " z ON z.id=g.last"
        " WHERE a.old IS NOT z.new ORDER BY a.id";
    static const char detail_query[] =
        "SELECT f.key,?1->f.key,?2->f.key,(?1->f.key) IS NOT (?2->f.key) FROM json_each(coalesce(?2,?1)) f"
        " WHERE f.key<>'action' AND (f.type<>'null' OR (?1->f.key) IS NOT NULL)"
        " AND (?1 IS NULL OR ?2 IS NULL OR (?1->f.key) IS NOT (?2->f.key) OR instr(f.key,'signature')>0)";
    sqlite3* db = state->db;
    sqlite3_stmt *st = NULL, *fields = NULL;
    int rc = sqlite3_prepare_v2(db, query, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int64(st, 1, state->emitted_batch);
    if (!rc) rc = sqlite3_bind_int64(st, 2, batch);
    if (!rc) rc = sqlite3_prepare_v2(db, detail_query, -1, &fields, NULL);
    struct strbuf line = {0};
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        char change[65];
        snprintf(change, sizeof(change), "%016llx/%lld/%lld", (unsigned long long)state->local.node,
            (long long)sqlite3_column_int64(st, 0), (long long)sqlite3_column_int64(st, 1));
        const char* specific = column(st, 8);
        if (strbuf_addf(&line, "%s group=%s record=%s %s change=%s%s%s", action(st), column(st, 3), column(st, 4), column(st, 5),
            change, *specific ? " action=" : "", specific) < 0) { rc = SQLITE_NOMEM; break; }
        rc = emit_details(fields, st, change, &line);
        if (rc) break;
    }
    strbuf_free(&line);
    int result = sqlite3_finalize(fields), final = sqlite3_finalize(st);
    return rc == SQLITE_DONE ? (result ? result : final) : rc;
}

void db_changes_flush(sqlite3* db) {
    struct db_changes* state = context(db);
    if (!state || !state->ready || !state->pending || !sqlite3_get_autocommit(db)) return;
    /* Через autocommit в очереди остаются только подтверждённые SQLite строки. */
    sqlite3_int64 batch = state->batch - 1;
    if (batch > state->emitted_batch && debug_should_output(DEBUG_LEVEL_INFO, LOG)) {
        int rc = emit(state, batch);
        if (rc) sql_failure(db, "read committed changes", NULL, rc);
    }
    state->emitted_batch = batch;
    int rc = sqlite3_exec(db, "DELETE FROM " AUDIT "." QUEUE, NULL, NULL, NULL);
    if (rc) { sql_failure(db, "clear committed changes", NULL, rc); return; }
    state->pending = 0;
}

static void flush_timer(void* data) {
    struct db_changes* state = data;
    if (*state->owner != state->db) return;
    db_changes_flush(state->db);
    if (state->pending && !sqlite3_get_autocommit(state->db)) uasync_timer_set(state->timer, 100);
}

static void destroy(void* data) {
    struct db_changes* state = data;
    if (state->timer) uasync_cancel_timeout(state->ua, state->timer);
    u_free(state);
}

void db_changes_close(sqlite3* db) {
    struct db_changes* state = context(db);
    if (!state) return;
    db_changes_flush(db);
    if (state->timer) {
        uasync_cancel_timeout(state->ua, state->timer);
        state->timer = NULL; state->owner = NULL;
    }
}

int db_changes_open(sqlite3* db, struct UASYNC* ua, sqlite3** owner, const uint8_t local[32], uint64_t node) {
    if (!db || !local || !!ua != !!owner || (owner && *owner != db)) {
        DEBUG_ERROR(LOG, "DB changes log requires a SQLite writer and its live uasync owner, or a worker");
        return SQLITE_MISUSE;
    }
    if (context(db)) return SQLITE_OK;
    struct db_changes* state = u_calloc(1, sizeof(*state));
    if (!state) { sql_failure(db, "allocate connection context", NULL, SQLITE_NOMEM); return SQLITE_NOMEM; }
    state->db = db; state->ua = ua; state->owner = owner; state->local.node = node; state->batch = 1;
    memcpy(state->local.ed, local, 32);
    int rc = sqlite3_set_clientdata(db, AUDIT, state, destroy);
    if (rc) { sql_failure(db, "register connection context", NULL, rc); return rc; }
    rc = sqlite3_exec(db, "ATTACH ':memory:' AS " AUDIT ";"
        "CREATE TABLE " AUDIT "." QUEUE "(id INTEGER PRIMARY KEY,batch INTEGER,kind TEXT,ch TEXT,record TEXT,origin TEXT,old TEXT,new TEXT);"
        "CREATE INDEX " AUDIT ".change_order ON " QUEUE "(batch,kind,ch,record,id)",
        NULL, NULL, NULL);
    if (rc) { sql_failure(db, "create change queue", NULL, rc); return rc; }
    if (ua) {
        state->timer = uasync_reserve_timer(ua, state, flush_timer, "db-changes");
        if (!state->timer) { sql_failure(db, "reserve flush timer", NULL, SQLITE_NOMEM); return SQLITE_NOMEM; }
    }
    static const struct { const char* name; int args; void (*call)(sqlite3_context*, int, sqlite3_value**); } functions[] = {
        {"db_changes_enabled",0,enabled}, {"db_changes_batch",0,batch}, {"db_changes_origin",0,sql_origin},
        {"db_changes_preview",1,preview}, {"db_changes_hash",1,payload_hash},
    };
    for (size_t i = 0; i < sizeof(functions)/sizeof(*functions); ++i) {
        rc = sqlite3_create_function_v2(db, functions[i].name, functions[i].args, SQLITE_UTF8, state, functions[i].call, NULL, NULL, NULL);
        if (rc) { sql_failure(db, "register SQL function", functions[i].name, rc); return rc; }
    }
    state->ready = 1;
    sqlite3_commit_hook(db, next_batch, state);
    sqlite3_stmt* st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT name FROM main.sqlite_schema WHERE type='table'", -1, &st, NULL);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) db_changes_watch(db, column(st, 0));
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) sql_failure(db, "enumerate watched tables", NULL, rc);
    return rc;
}
