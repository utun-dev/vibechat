#include "kanban_priv.h"
#include "kanban_read.h"

struct revision { struct kb_op op; struct kb_fields f; uint8_t* wire; int valid, applied; size_t depth, current, order; };
struct entity {
    char id[65], column[65]; const char *name, *color;
    uint8_t author[32]; size_t root, position, title, color_revision, ownership, due;
    int is_column, deleted, builtin;
};
struct board {
    struct revision* revisions; size_t revisions_count;
    struct entity* entities; size_t entities_count;
    size_t* order; size_t pending;
};
static void release(struct board* b) {
    for (size_t i = 0; i < b->revisions_count; i++) {
        kb_op_free(&b->revisions[i].op); kb_fields_free(&b->revisions[i].f); u_free(b->revisions[i].wire);
    }
    u_free(b->revisions); u_free(b->entities); u_free(b->order); memset(b, 0, sizeof(*b));
}
static size_t find_entity(const struct board* b, const char* id) {
    for (size_t i = 0; i < b->entities_count; i++) if (!strcmp(b->entities[i].id, id)) return i;
    return SIZE_MAX;
}
static size_t find_revision(const struct board* b, const uint8_t id[32]) {
    for (size_t i = 0; i < b->revisions_count; i++) if (!memcmp(b->revisions[i].op.id, id, 32)) return i;
    return SIZE_MAX;
}
static int add_entity(struct board* b, const char* id, const char* column, const char* name, const uint8_t* author,
    int is_column, size_t root, int builtin) {
    if (b->entities_count >= UINT32_MAX / sizeof(*b->entities) - 1) return -1;
    struct entity* e = u_realloc(b->entities, (b->entities_count + 1) * sizeof(*e));
    if (!e) return -1;
    b->entities = e;
    size_t* order = u_realloc(b->order, (b->entities_count + 1) * sizeof(*order));
    if (!order) return -1;
    b->order = order; e = &b->entities[b->entities_count]; memset(e, 0, sizeof(*e));
    strcpy(e->id, id); strcpy(e->column, column); e->name = name; if (author) memcpy(e->author, author, 32); e->is_column = is_column;
    e->root = root; e->position = root; e->title = root; e->builtin = builtin;
    e->ownership = root; e->due = SIZE_MAX;
    e->color_revision = root != SIZE_MAX && *b->revisions[root].f.color ? root : SIZE_MAX;
    e->color = root != SIZE_MAX && *b->revisions[root].f.color ? b->revisions[root].f.color : kb_color(0);
    b->order[b->entities_count] = b->entities_count; b->entities_count++; return 0;
}
static int newer(const struct board* b, size_t incoming, size_t previous) {
    if (previous == SIZE_MAX) return 1;
    const struct kb_op *a = &b->revisions[incoming].op, *c = &b->revisions[previous].op;
    return a->time > c->time || (a->time == c->time && memcmp(a->key, c->key, 32) > 0);
}

/* Recovery содержит group-подпись и точный срез истории прежнего автора.
 * Срез остаётся проверяемым после реактивации; отсутствие wire — зависимость, не отказ прав. */
static int recovery_cutoff(sqlite3* db, const char* group, const uint8_t owner[32], const char* cutoff) {
    uint8_t id[32]; sqlite3_stmt* st = NULL;
    if (kb_unhex(cutoff, id)) return -1;
    int rc = sqlite3_prepare_v2(db, "SELECT wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2 AND action=9", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int result = 0; struct kb_op seal = {0}; struct kb_fields fields = {0};
    if (rc == SQLITE_ROW) {
        if (kb_decode(db, group, sqlite3_column_blob(st, 0), sqlite3_column_bytes(st, 0), &seal) ||
            kb_fields_read(db, seal.body, &fields)) result = -1;
        else result = seal.group_signed && !memcmp(fields.target_ed, owner, 32) ? 1 : 2;
    } else if (rc != SQLITE_DONE) result = -1;
    int final = sqlite3_finalize(st); kb_op_free(&seal); kb_fields_free(&fields);
    if (result < 0 || final != SQLITE_OK) return kb_error(db, group, "recovery cutoff READ");
    return result;
}

/* Детерминированный причинный replay: общий допуск уже проверен reindex.
 * applied: 0=ждём зависимость, 1=применено, -1=отказ. Текстовый DAG независим. */
static int details(sqlite3* db, const char* group, struct board* b) {
    size_t order = 0;
    for (size_t i = 0; i < b->revisions_count; i++) {
        struct revision* r = &b->revisions[i];
        r->current = i;
        if (r->op.action == KB_CARD_CREATE) r->applied = 1;
    }
    for (;;) {
        size_t next = SIZE_MAX;
        for (size_t i = 0; i < b->revisions_count; i++) {
            struct revision* r = &b->revisions[i];
            if (r->applied || (r->op.action < KB_CARD_AUTHOR && r->op.action != KB_CARD_DELETE)) continue;
            size_t e = find_entity(b, r->f.entity);
            if (e == SIZE_MAX || b->entities[e].is_column) continue;
            if (r->op.action == KB_CARD_AUTHOR || r->op.action == KB_ASSIGN) {
                int found = kb_member_access(db, group, r->f.target_ed, 0);
                if (found < 0) return -1;
                if (!found) continue;
            }
            if (*r->f.cutoff) {
                int found = recovery_cutoff(db, group, b->entities[e].author, r->f.cutoff);
                if (found < 0) return -1;
                if (!found) continue;
            }
            if (*r->f.grant) {
                uint8_t id[32]; kb_unhex(r->f.grant, id); size_t dependency = find_revision(b, id);
                if (dependency == SIZE_MAX || !b->revisions[dependency].applied) continue;
            }
            if (r->op.action == KB_MESSAGE_EDIT || r->op.action == KB_MESSAGE_DELETE) {
                uint8_t id[32]; kb_unhex(r->f.before, id); size_t dependency = find_revision(b, id);
                if (dependency == SIZE_MAX || !b->revisions[dependency].applied) continue;
            }
            next = i; break; /* load уже отсортирован time/Ed25519/id. */
        }
        if (next == SIZE_MAX) break;
        struct revision* r = &b->revisions[next]; struct entity* e = &b->entities[find_entity(b, r->f.entity)];
        char epoch[65]; kb_hex(b->revisions[e->ownership].op.id, epoch);
        int owner = !memcmp(e->author, r->op.key, 32);
        int recovery = *r->f.cutoff ? recovery_cutoff(db, group, e->author, r->f.cutoff) : 0;
        if (recovery < 0) return -1;
        int privileged = owner || (r->op.group_signed && recovery == 1);
        int allowed = !e->deleted;
        if (r->op.action == KB_CARD_AUTHOR || r->op.action == KB_CARD_DUE ||
            ((r->op.action == KB_ASSIGN || r->op.action == KB_UNASSIGN) && memcmp(r->f.target_ed, r->op.key, 32)))
            allowed &= privileged && !strcmp(epoch, r->f.grant);
        if (r->op.action == KB_CARD_DELETE) {
            struct kb_op moderator = r->op;
            moderator.body = "{\"action\":\"card_delete\",\"entity\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
            int moderation = kb_delete_allowed(db, group, &moderator);
            if (moderation < 0) return -1;
            allowed &= owner || moderation;
        }
        if (r->op.action == KB_MESSAGE_EDIT || r->op.action == KB_MESSAGE_DELETE) {
            uint8_t id[32]; kb_unhex(r->f.before, id); size_t root = find_revision(b, id);
            struct revision* message = &b->revisions[root];
            allowed &= message->op.action == KB_MESSAGE_CREATE && message->applied == 1 &&
                !memcmp(message->op.key, r->op.key, 32) && !strcmp(message->f.entity, r->f.entity) &&
                b->revisions[message->current].op.action != KB_MESSAGE_DELETE;
            if (allowed) message->current = next;
        }
        r->applied = allowed ? 1 : -1;
        r->order = ++order;
        if (!allowed) {
            DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "card operation has no effect ch=%s card=%s action=%d signer=%s",
                group, r->f.entity, r->op.action, sc_public_key_hex(r->op.key, (char[65]){0}));
            continue;
        }
        if (r->op.action == KB_CARD_AUTHOR) { memcpy(e->author, r->f.target_ed, 32); e->ownership = next; }
        if (r->op.action == KB_CARD_DUE) e->due = next;
        if (r->op.action == KB_CARD_DELETE) e->deleted = 1;
    }
    for (size_t i = 0; i < b->revisions_count; i++) {
        const struct revision* r = &b->revisions[i];
        if (!r->applied && (r->op.action >= KB_CARD_AUTHOR || r->op.action == KB_CARD_DELETE)) b->pending++;
    }
    return 0;
}
static void move(struct board* b, size_t entity, const char* before) {
    size_t at = 0; while (at < b->entities_count && b->order[at] != entity) at++;
    if (at == b->entities_count) return;
    memmove(b->order + at, b->order + at + 1, (b->entities_count - at - 1) * sizeof(*b->order));
    size_t target = b->entities_count - 1, anchor = find_entity(b, before);
    const struct entity *e = &b->entities[entity], *a = anchor == SIZE_MAX ? NULL : &b->entities[anchor];
    if (a && anchor != entity && !a->deleted && a->is_column == e->is_column && (e->is_column || !strcmp(a->column, e->column))) {
        target = 0; while (target < b->entities_count - 1 && b->order[target] != anchor) target++;
    }
    memmove(b->order + target + 1, b->order + target, (b->entities_count - target - 1) * sizeof(*b->order));
    b->order[target] = entity;
}
static int load(sqlite3* db, const char* group, struct board* b) {
    static const char* names[] = {"New", "In progress", "Done"};
    for (unsigned i = 0; i < 3; i++) {
        char domain[96], id[65]; uint8_t hash[32];
        int bytes = snprintf(domain, sizeof(domain), "utun.kanban.column.v1:%s:%u", group, i);
        if (EVP_Digest(domain, (size_t)bytes, hash, NULL, EVP_sha256(), NULL) != 1) goto fail;
        kb_hex(hash, id); if (add_entity(b, id, "", names[i], 0, 1, SIZE_MAX, (int)i + 1)) goto fail;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND active=1 AND action<>9"
        " ORDER BY time,author,id", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) { sqlite3_finalize(st); goto fail; }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (b->revisions_count >= UINT32_MAX / sizeof(*b->revisions) - 1) { rc = SQLITE_NOMEM; break; }
        struct revision* rows = u_realloc(b->revisions, (b->revisions_count + 1) * sizeof(*rows));
        if (!rows) { rc = SQLITE_NOMEM; break; } b->revisions = rows;
        struct revision* r = &rows[b->revisions_count++]; memset(r, 0, sizeof(*r));
        int size = sqlite3_column_bytes(st, 0);
        if (size < KB_WIRE_FIXED || size > KB_WIRE_MAX || !(r->wire = u_malloc((uint32_t)size))) { rc = SQLITE_CORRUPT; break; }
        memcpy(r->wire, sqlite3_column_blob(st, 0), (size_t)size);
        if (kb_decode(db, group, r->wire, (size_t)size, &r->op) || kb_fields_read(db, r->op.body, &r->f)) { rc = SQLITE_CORRUPT; break; }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) goto fail;
    for (size_t i = 0; i < b->revisions_count; i++) {
        struct revision* r = &b->revisions[i];
        if (r->op.action != KB_CARD_CREATE && r->op.action != KB_COLUMN_CREATE) continue;
        char id[65]; kb_hex(r->op.id, id);
        if (add_entity(b, id, r->f.column, r->f.text, r->op.key, r->op.action == KB_COLUMN_CREATE, i, 0)) goto fail;
        r->valid = 1;
    }
    /* Причинный порядок текста независим от часов: часы нужны только положению. */
    int advanced;
    do {
        advanced = 0;
        for (size_t i = 0; i < b->revisions_count; i++) {
            struct revision* r = &b->revisions[i];
            if (r->valid || r->op.action != KB_CARD_EDIT || !r->f.parents_count) continue;
            size_t entity = find_entity(b, r->f.entity), depth = 0; int valid = entity != SIZE_MAX && !b->entities[entity].is_column;
            for (size_t j = 0; valid && j < r->f.parents_count; j++) {
                size_t p = find_revision(b, r->f.parents[j]);
                if (p == SIZE_MAX || !b->revisions[p].valid ||
                    (p != b->entities[entity].root && strcmp(b->revisions[p].f.entity, r->f.entity))) valid = 0;
                else if (b->revisions[p].depth >= depth) depth = b->revisions[p].depth + 1;
            }
            if (valid) { r->valid = 1; r->depth = depth; advanced = 1; }
        }
    } while (advanced);
    for (size_t i = 0; i < b->revisions_count; i++) {
        struct revision* r = &b->revisions[i]; size_t e = find_entity(b, r->f.entity);
        if (r->op.action == KB_CARD_EDIT && !r->valid) { b->pending++; continue; }
        if (e == SIZE_MAX) continue;
        struct entity* entity = &b->entities[e];
        if (r->op.action == KB_CARD_EDIT && *r->f.color && newer(b, i, entity->color_revision)) {
            entity->color = r->f.color; entity->color_revision = i;
        }
        if (r->op.action == KB_COLUMN_DELETE && entity->is_column) {
            if (entity->is_column) {
                size_t live = 0;
                for (size_t j = 0; j < b->entities_count; j++) live += b->entities[j].is_column && !b->entities[j].deleted;
                if (live <= 1) continue;
            }
            entity->deleted = 1;
        }
        if (r->op.action == KB_COLUMN_RENAME && entity->is_column && newer(b, i, entity->title)) {
            entity->name = r->f.text; entity->title = i; entity->builtin = 0;
        }
    }
    if (details(db, group, b)) goto fail;
    /* Создание и перемещения проигрываются вместе: старое «в конец» не обгоняет новую карточку. */
    for (size_t i = 0; i < b->revisions_count; i++) {
        struct revision* r = &b->revisions[i];
        int creating = r->op.action == KB_CARD_CREATE || r->op.action == KB_COLUMN_CREATE;
        char id[65]; if (creating) kb_hex(r->op.id, id);
        size_t e = find_entity(b, creating ? id : r->f.entity);
        if (e == SIZE_MAX) continue;
        struct entity* entity = &b->entities[e];
        if (creating || (((r->op.action == KB_CARD_MOVE && !entity->is_column) ||
            (r->op.action == KB_COLUMN_MOVE && entity->is_column)) && newer(b, i, entity->position))) {
            if (!entity->is_column) strcpy(entity->column, r->f.column);
            entity->position = i; move(b, e, r->f.before);
            DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "position replay ch=%s entity=%s action=%d column=%s before=%s time=%llu",
                group, entity->id, r->op.action, entity->column, r->f.before, (unsigned long long)r->op.time);
        }
    }
    const char* fallback = NULL;
    for (size_t i = 0; i < b->entities_count; i++) {
        const struct entity* e = &b->entities[b->order[i]];
        if (e->is_column && !e->deleted) { fallback = e->id; break; }
    }
    if (!fallback) goto fail;
    for (size_t i = 0; i < b->entities_count; i++) if (!b->entities[i].is_column) {
        size_t c = find_entity(b, b->entities[i].column);
        if (c == SIZE_MAX || !b->entities[c].is_column || b->entities[c].deleted) {
            strcpy(b->entities[i].column, fallback); move(b, i, "");
        }
    }
    if (b->pending) DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "board awaits operation dependencies ch=%s revisions=%zu pending=%zu",
        group, b->revisions_count, b->pending);
    return 0;
fail:
    release(b); return kb_error(db, group, "board projection");
}

static void ancestors(const struct board* b, size_t root, uint8_t* seen, size_t* stack) {
    size_t count = 0; stack[count++] = root; seen[root] = 1;
    while (count) {
        const struct revision* r = &b->revisions[stack[--count]];
        for (size_t i = 0; i < r->f.parents_count; i++) {
            size_t p = find_revision(b, r->f.parents[i]);
            if (p != SIZE_MAX && !seen[p]) { seen[p] = 1; stack[count++] = p; }
        }
    }
}
static int card_text(const struct board* b, size_t e, char** text, size_t** heads_out, size_t* count_out) {
    size_t n = b->revisions_count; int result = -1;
    uint8_t* child = u_calloc(n ? n : 1, 1); uint8_t* seen = u_calloc(n ? n : 1, 1);
    size_t* heads = u_calloc(n ? n : 1, sizeof(*heads)); size_t* common = u_calloc(n ? n : 1, sizeof(*common));
    size_t* stack = u_calloc(n ? n : 1, sizeof(*stack)); size_t count = 0;
    if (!child || !seen || !heads || !common || !stack) goto done;
    for (size_t i = 0; i < n; i++) if (b->revisions[i].valid) {
        const struct revision* r = &b->revisions[i];
        for (size_t j = 0; j < r->f.parents_count; j++) {
            size_t p = find_revision(b, r->f.parents[j]); if (p != SIZE_MAX) child[p] = 1;
        }
    }
    for (size_t i = 0; i < n; i++) if (b->revisions[i].valid && !child[i] &&
        (i == b->entities[e].root || !strcmp(b->revisions[i].f.entity, b->entities[e].id))) {
        size_t j = count;
        while (j && memcmp(b->revisions[heads[j - 1]].op.id, b->revisions[i].op.id, 32) > 0) { heads[j] = heads[j - 1]; j--; }
        heads[j] = i; count++;
    }
    if (!count) goto done;
    size_t base = b->entities[e].root;
    for (size_t i = 0; i < count; i++) {
        memset(seen, 0, n); ancestors(b, heads[i], seen, stack);
        for (size_t j = 0; j < n; j++) common[j] += seen[j];
    }
    for (size_t i = 0; i < n; i++) if (common[i] == count && (b->revisions[i].depth > b->revisions[base].depth ||
        (b->revisions[i].depth == b->revisions[base].depth && memcmp(b->revisions[i].op.id, b->revisions[base].op.id, 32) > 0))) base = i;
    *text = u_strdup(b->revisions[heads[0]].f.text); if (!*text) goto done;
    result = 0;
    for (size_t i = 1; i < count; i++) {
        char* merged = NULL;
        int rc = kanban_text_merge(b->revisions[base].f.text, *text, b->revisions[heads[i]].f.text, &merged);
        if (rc < 0) { result = -1; goto done; }
        if (rc > 0 || strlen(merged) > KANBAN_TEXT_MAX) {
            u_free(merged); u_free(*text); *text = u_strdup(b->revisions[heads[0]].f.text);
            result = *text ? 1 : -1; break;
        }
        u_free(*text); *text = merged;
    }
    if (result >= 0) { *heads_out = heads; *count_out = count; heads = NULL; }
done:
    u_free(child); u_free(seen); u_free(heads); u_free(common); u_free(stack);
    if (result < 0) { u_free(*text); *text = NULL; DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "text DAG projection failed entity=%s", b->entities[e].id); }
    return result;
}

int kb_build_snapshot(sqlite3* db, const char* group, const uint8_t self[32], char** result) {
    if (result) *result = NULL;
    if (!result || kb_type(db, group)) return -1;
    struct board b = {0}; struct strbuf json = {0};
    if (load(db, group, &b)) return -1;
    uint8_t key[32]; int member = kb_member_access(db, group, self, 1);
    if (member < 0) { release(&b); return member; }
    int manage = member && !topo_node_sqlite_channel_get_priv(db, group, key); OPENSSL_cleanse(key, sizeof(key));
    struct kb_op permission = {.group_signed = manage,
        .body = "{\"action\":\"card_delete\",\"entity\":\"0000000000000000000000000000000000000000000000000000000000000000\"}"};
    memcpy(permission.key, self, 32);
    int moderate = member ? kb_delete_allowed(db, group, &permission) : 0;
    int failed = moderate < 0 || strbuf_addf(&json,
        "{\"can_manage\":%s,\"can_write\":%s,\"can_moderate\":%s,\"self\":\"%s\",\"pending\":%zu,\"columns\":[",
        manage ? "true" : "false", member ? "true" : "false", moderate > 0 ? "true" : "false", sc_public_key_hex(self, (char[65]){0}), b.pending) < 0;
    size_t written = 0;
    for (size_t i = 0; !failed && i < b.entities_count; i++) {
        const struct entity* e = &b.entities[b.order[i]]; if (!e->is_column) continue;
        failed = strbuf_addf(&json, "%s{\"id\":\"%s\",\"deleted\":%s,\"builtin\":%d,\"name\":",
            written++ ? "," : "", e->id, e->deleted ? "true" : "false", e->builtin) < 0 || strbuf_add_json_string(&json, e->name) ||
            strbuf_addf(&json, "}") < 0;
    }
    if (strbuf_addf(&json, "],\"cards\":[") < 0) failed = 1;
    written = 0;
    for (size_t i = 0; !failed && i < b.entities_count; i++) {
        size_t index = b.order[i]; const struct entity* e = &b.entities[index]; if (e->is_column) continue;
        char* text = NULL; size_t* heads = NULL, count = 0; int conflict = card_text(&b, index, &text, &heads, &count);
        if (conflict < 0) { failed = 1; break; }
        failed = strbuf_addf(&json, "%s{\"id\":\"%s\",\"column\":\"%s\",\"author\":\"%s\",\"deleted\":%s,\"conflict\":%s,\"text\":",
            written++ ? "," : "", e->id, e->column, sc_public_key_hex(e->author, (char[65]){0}), e->deleted ? "true" : "false", conflict ? "true" : "false") < 0 ||
            strbuf_add_json_string(&json, text) || strbuf_addf(&json, ",\"color\":\"%s\",\"heads\":[", e->color) < 0;
        for (size_t j = 0; !failed && j < count; j++) {
            char id[65]; kb_hex(b.revisions[heads[j]].op.id, id); failed = strbuf_addf(&json, "%s\"%s\"", j ? "," : "", id) < 0;
        }
        if (strbuf_addf(&json, "],\"variants\":[") < 0) failed = 1;
        for (size_t j = 0; conflict && !failed && j < count; j++) {
            const struct revision* r = &b.revisions[heads[j]];
            failed = strbuf_addf(&json, "%s{\"author\":\"%s\",\"text\":", j ? "," : "", sc_public_key_hex(r->op.key, (char[65]){0})) < 0 ||
                strbuf_add_json_string(&json, r->f.text) || strbuf_addf(&json, "}") < 0;
        }
        char grant[65]; kb_hex(b.revisions[e->ownership].op.id, grant);
        const struct kb_op* creation = &b.revisions[e->root].op;
        int author_active = kb_member_access(db, group, e->author, 1);
        if (author_active < 0) failed = 1;
        if (strbuf_addf(&json, "],\"creator\":\"%s\",\"created_at\":%llu,\"grant\":\"%s\",\"due\":",
            sc_public_key_hex(creation->key, (char[65]){0}), (unsigned long long)creation->time, grant) < 0 ||
            strbuf_add_json_string(&json, e->due == SIZE_MAX ? "" : b.revisions[e->due].f.text) ||
            strbuf_addf(&json, ",\"author_active\":%s,\"assignees\":[", author_active ? "true" : "false") < 0) failed = 1;
        size_t assigned = 0;
        for (size_t j = 0; !failed && j < b.revisions_count; j++) {
            const struct revision* r = &b.revisions[j];
            if (r->applied != 1 || r->op.action != KB_ASSIGN || strcmp(r->f.entity, e->id)) continue;
            int last = 1;
            for (size_t k = 0; k < b.revisions_count; k++) {
                const struct revision* later = &b.revisions[k];
                if (later->applied == 1 && (later->op.action == KB_ASSIGN || later->op.action == KB_UNASSIGN) &&
                    later->order > r->order && !memcmp(later->f.target_ed, r->f.target_ed, 32) && !strcmp(later->f.entity, e->id)) last = 0;
            }
            if (last && strbuf_addf(&json, "%s\"%s\"", assigned++ ? "," : "", sc_public_key_hex(r->f.target_ed, (char[65]){0})) < 0) failed = 1;
        }
        if (strbuf_addf(&json, "],\"_assignments\":[") < 0) failed = 1;
        size_t assignment_rows = 0;
        for (size_t j = 0; !failed && j < b.revisions_count; j++) {
            const struct revision* r = &b.revisions[j];
            if (r->applied != 1 || (r->op.action != KB_ASSIGN && r->op.action != KB_UNASSIGN) || strcmp(r->f.entity, e->id)) continue;
            const struct revision* first = r; int last = 1;
            for (size_t k = 0; k < b.revisions_count; k++) {
                const struct revision* other = &b.revisions[k];
                if (other->applied != 1 || (other->op.action != KB_ASSIGN && other->op.action != KB_UNASSIGN) ||
                    memcmp(other->f.target_ed, r->f.target_ed, 32) || strcmp(other->f.entity, e->id)) continue;
                if (other->order > r->order) last = 0;
                if (other->order < first->order) first = other;
            }
            if (!last) continue;
            char revision[65]; kb_hex(r->op.id, revision);
            failed = strbuf_addf(&json, "%s{\"id\":\"%s\",\"created_by\":\"%s\",\"created_at\":%llu,"
                "\"updated_by\":\"%s\",\"updated_at\":%llu,\"revision\":\"%s\",\"deleted\":%s}",
                assignment_rows++ ? "," : "", sc_public_key_hex(r->f.target_ed, (char[65]){0}), sc_public_key_hex(first->op.key, (char[65]){0}),
                (unsigned long long)first->op.time, sc_public_key_hex(r->op.key, (char[65]){0}), (unsigned long long)r->op.time,
                revision, r->op.action == KB_UNASSIGN ? "true" : "false") < 0;
        }
        if (strbuf_addf(&json, "],\"messages\":[") < 0) failed = 1;
        size_t messages = 0;
        for (size_t j = 0; !failed && j < b.revisions_count; j++) {
            const struct revision* r = &b.revisions[j];
            if (r->applied != 1 || r->op.action != KB_MESSAGE_CREATE || strcmp(r->f.entity, e->id)) continue;
            const struct revision* current = &b.revisions[r->current]; char id[65], revision[65];
            kb_hex(r->op.id, id); kb_hex(current->op.id, revision);
            int deleted = current->op.action == KB_MESSAGE_DELETE;
            failed = strbuf_addf(&json, "%s{\"id\":\"%s\",\"author\":\"%s\",\"created_at\":%llu,"
                "\"updated_at\":%llu,\"revision\":\"%s\",\"deleted\":%s,\"text\":", messages++ ? "," : "", id,
                sc_public_key_hex(r->op.key, (char[65]){0}), (unsigned long long)r->op.time,
                (unsigned long long)current->op.time, revision, deleted ? "true" : "false") < 0 ||
                strbuf_add_json_string(&json, deleted || !current->f.text ? "" : current->f.text);
            if (!failed && r->f.attachment)
                failed = strbuf_addf(&json, ",\"ct\":\"%s\",\"d\":", r->f.content_type) < 0 ||
                    strbuf_add_json_string(&json, r->f.attachment);
            if (strbuf_addf(&json, "}") < 0) failed = 1;
        }
        if (strbuf_addf(&json, "]}") < 0) failed = 1;
        u_free(text); u_free(heads);
    }
    if (strbuf_addf(&json, "],\"colors\":[") < 0) failed = 1;
    for (unsigned i = 0; !failed && i < KB_COLOR_COUNT; i++)
        failed = strbuf_addf(&json, "%s\"%s\"", i ? "," : "", kb_color(i)) < 0;
    if (strbuf_addf(&json, "]}") < 0) failed = 1;
    release(&b);
    if (!failed) *result = strbuf_detach(&json);
    strbuf_free(&json);
    return *result ? 0 : kb_error(db, group, "snapshot allocation/serialization");
}

int kanban_history(struct UTUN_INSTANCE* inst, const char* group, const char* entity, char** result) {
    if (result) *result = NULL;
    uint8_t id[32];
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!result || kb_type(db, group) || kb_unhex(entity, id)) return kb_error(db, group, "history arguments");
    return kanban_history_read(inst->topo_sqlite_db, group, entity, result) == SQLITE_OK
        ? 0 : kb_error(db, group, "history serialization");
}
