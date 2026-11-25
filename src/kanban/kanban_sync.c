#include "kanban_priv.h"
#include "../db_changes.h"
#include "../group/group_protocol.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp_connections.h"
#include "../transport_layer/etcp.h"

struct kb_pull { char group[65]; uint8_t id[16]; int changed; }; /* Собственные SQL/crypto входы, без runtime pointers. */

static int eligible(void* ctx, const char* group, struct ETCP_CONN* conn) {
    struct UTUN_INSTANCE* inst = ctx;
    if (!inst->kanban_started) return 0;
    struct TOPO_GROUP* topology = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, group) : NULL;
    return group_protocol_current(inst, group) == GROUP_PROTOCOL_KANBAN && topo_group_chat_connection_allowed(topology, conn) &&
        topo_group_connection_ready(topology, conn) && conn && conn->initialized && conn->links_up && !conn->close_requested;
}

static int leaf(void* ctx, const char* group, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    struct UTUN_INSTANCE* inst = ctx;
    return kb_leaf_hash(inst->topo_sqlite_db, group, level, prefix, hash);
}

static int kb_page_read(sqlite3* db, const char* group, uint64_t prefix, int after_valid, const merkle_key_t* after,
    uint8_t* out, size_t* len, merkle_key_t* next, int* more) {
    sqlite3_stmt* st = NULL; size_t used = 0; *more = 0; memset(next, 0, sizeof(*next));
    int rc = sqlite3_prepare_v2(db, "SELECT id,wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND bucket=?2"
        " AND active=1 AND (?3=0 OR id>?4) ORDER BY id", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(prefix >> 39));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 3, after_valid);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, after->bytes, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int size = sqlite3_column_bytes(st, 1);
        if (size < KB_WIRE_FIXED || size > KB_WIRE_MAX || sqlite3_column_bytes(st, 0) != 32) { rc = SQLITE_CORRUPT; break; }
        const void* id = sqlite3_column_blob(st, 0); const void* wire = sqlite3_column_blob(st, 1);
        if (!id || !wire) { rc = SQLITE_NOMEM; break; }
        if ((size_t)size + 2 > *len - used) {
            if (!used) rc = SQLITE_TOOBIG;
            else { *more = 1; rc = SQLITE_DONE; }
            break;
        }
        out[used++] = (uint8_t)(size >> 8); out[used++] = (uint8_t)size;
        memcpy(out + used, wire, (size_t)size); used += (size_t)size; memcpy(next->bytes, id, 32);
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        int code = rc == SQLITE_DONE ? final : rc;
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board page read ch=%s rc=%d finalize=%d", group, code, final); return -code;
    }
    *len = used; return 0;
}
static int page(void* ctx, const char* group, uint64_t prefix, int after_valid, const merkle_key_t* after,
    uint8_t* out, size_t* len, merkle_key_t* next, int* more) {
    return kb_page_read(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, group, prefix, after_valid, after, out, len, next, more);
}

static int kb_begin_write(sqlite3* db, const char* group, void** out) {
    *out = NULL;
    struct kb_pull* pull = u_calloc(1, sizeof(*pull));
    if (!pull) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board staging allocation ch=%s", group); return -SQLITE_NOMEM; }
    if (!chat_channel_valid(group) || RAND_bytes(pull->id, sizeof(pull->id)) != 1) {
        u_free(pull); return kb_error(db, group, "staging namespace/random token");
    }
    strcpy(pull->group, group);
    int code = kb_exec(db, "CREATE TEMP TABLE IF NOT EXISTS kanban_stage(pull BLOB NOT NULL CHECK(length(pull)=16),"
        "id BLOB NOT NULL,wire BLOB NOT NULL,PRIMARY KEY(pull,id))");
    if (code) { u_free(pull); return code; }
    *out = pull; return 0;
}
static int begin(void* ctx, const char* group, const uint8_t peer[32], uint64_t round, void** out) {
    (void)peer; (void)round; return kb_begin_write(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, group, out);
}

static int kb_stage_write(sqlite3* db, struct kb_pull* pull, uint64_t prefix, int after_valid, const merkle_key_t* after,
    const merkle_key_t* next, int more, const uint8_t* data, size_t len) {
    sqlite3_stmt* st = NULL; int code = SQLITE_MISMATCH;
    uint8_t previous[32] = {0}; if (after_valid) memcpy(previous, after->bytes, 32);
    size_t offset = 0; int count = 0;
    const char* reason = "invalid item length";
    while (offset < len) {
        if (len - offset < 2) goto fail;
        size_t size = (size_t)data[offset] * 256 + data[offset + 1]; offset += 2;
        struct kb_op op = {0};
        if (size > len - offset) goto fail;
        if (kb_decode(db, pull->group, data + offset, size, &op)) { reason = NULL; goto fail; }
        if (merkle_sync_level_prefix(kb_read64(op.id), MT_MAX_LEVEL) != prefix ||
            ((count || after_valid) && memcmp(op.id, previous, 32) <= 0)) {
            db_changes_warn(db, NULL, pull->group, "Kanban operation", "invalid Merkle position", op.key, op.wire + op.size - 128, NULL);
            kb_op_free(&op); reason = NULL; goto fail;
        }
        memcpy(previous, op.id, 32); count++;
        int rc = sqlite3_prepare_v2(db, "INSERT INTO kanban_stage(pull,id,wire) VALUES(?1,?2,?3)"
            " ON CONFLICT(pull,id) DO NOTHING", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, pull->id, sizeof(pull->id), SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, op.id, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, op.wire, (int)op.size, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int final = sqlite3_finalize(st); st = NULL; kb_op_free(&op);
        if (rc != SQLITE_DONE || final != SQLITE_OK) { code = rc != SQLITE_DONE ? rc : final; reason = NULL; goto fail; }
        offset += size;
    }
    reason = "invalid page cursor";
    if (memcmp(previous, next->bytes, 32) || (!count && more)) goto fail;
    return 0;
fail:
    if (reason) db_changes_warn(db, NULL, pull->group, "Kanban Merkle page", reason, NULL, NULL, NULL);
    sqlite3_finalize(st); DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "sync page rejected ch=%s offset=%zu bytes=%zu", pull->group, offset, len);
    return -code;
}
static int stage(void* ctx, void* state, uint64_t prefix, int after_valid, const merkle_key_t* after,
    const merkle_key_t* next, int more, const uint8_t* data, size_t len) {
    int code = kb_stage_write(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, state, prefix, after_valid, after, next, more, data, len);
    return code < 0 ? MT_ERR_DATA : 0;
}

static int kb_commit_write(sqlite3* db, const uint8_t self[32], struct kb_pull* pull) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board receive requires caller WRITE"); return -SQLITE_MISUSE; }
    int rc = sqlite3_prepare_v2(db, "SELECT wire FROM kanban_stage WHERE pull=?1 ORDER BY id", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, pull->id, sizeof(pull->id), SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct kb_op op = {0};
        if (kb_decode(db, pull->group, sqlite3_column_blob(st, 0), sqlite3_column_bytes(st, 0), &op)) { rc = SQLITE_CORRUPT; break; }
        int inserted = kb_store(db, pull->group, &op); kb_op_free(&op);
        if (inserted < 0) { rc = -inserted; break; }
        pull->changed |= inserted;
    }
    int final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_DONE || final != SQLITE_OK) { if (rc == SQLITE_DONE) rc = final; goto fail; }
    /* Все части среза уже находятся в той же транзакции: порядок страниц не важен. */
    rc = sqlite3_prepare_v2(db, "SELECT wire FROM kanban_stage WHERE pull=?1 ORDER BY id", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, pull->id, sizeof(pull->id), SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct kb_op op = {0};
        if (kb_decode(db, pull->group, sqlite3_column_blob(st, 0), sqlite3_column_bytes(st, 0), &op)) { rc = SQLITE_CORRUPT; break; }
        int allowed = kb_authorized(db, pull->group, &op); kb_op_free(&op);
        if (allowed != 1) { rc = allowed < 0 ? -allowed : SQLITE_AUTH; break; }
    }
    final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_DONE || final != SQLITE_OK) { if (rc == SQLITE_DONE) rc = final; goto fail; }
    rc = kb_reindex(db, pull->group, self); if (rc < 0) { rc = -rc; goto fail; }
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "board receive staged ch=%s changed=%d; commit pending", pull->group, pull->changed); return 0;
fail:
    final = sqlite3_finalize(st);
    DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board receive failed ch=%s rc=%d finalize=%d: %s", pull->group, rc, final, sqlite3_errstr(rc));
    return -rc;
}
static int commit(void* ctx, void* state) {
    struct UTUN_INSTANCE* inst = ctx; sqlite3* db = inst->topo_sqlite_db;
    if (!sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board legacy receive foreign transaction"); return MT_ERR_IO; }
    int rc = kb_exec(db, "BEGIN IMMEDIATE /* kanban sync */");
    if (!rc) rc = kb_commit_write(db, inst->my_ed25519_pubkey, state);
    if (!rc) rc = kb_exec(db, "COMMIT /* kanban sync */");
    if (rc && !sqlite3_get_autocommit(db) && kb_exec(db, "ROLLBACK /* kanban sync */")) utun_instance_stop(inst);
    return rc < 0 ? MT_ERR_DATA : 0;
}

static void publish(void* ctx, void* state) {
    struct kb_pull* pull = state; if (pull->changed) kb_publish(ctx, pull->group);
}
static int kb_abort_write(sqlite3* db, struct kb_pull* pull) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "DELETE FROM kanban_stage WHERE pull=?1", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, pull->id, sizeof(pull->id), SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        int code = rc == SQLITE_DONE ? final : rc;
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board staging cleanup failed ch=%s rc=%d finalize=%d", pull->group, code, final); return -code;
    }
    return 0;
}
static void abort_pull(void* ctx, void* state) {
    kb_abort_write(((struct UTUN_INSTANCE*)ctx)->topo_sqlite_db, state); u_free(state);
}
static void result(void* ctx, const char* group, const uint8_t peer[32], int status) {
    (void)ctx;
    if (status == MT_OK) DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "board synchronized ch=%s peer=%s", group, sc_public_key_hex(peer, (char[65]){0}));
    else DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "board sync failed ch=%s peer=%s result=%d", group, sc_public_key_hex(peer, (char[65]){0}), status);
}
struct kb_sql_scope { uint8_t self[32]; uint64_t generation; };
static int kb_sql_snapshot(void* ctx, const char* group, void** out) {
    struct UTUN_INSTANCE* inst = ctx; *out = NULL;
    if (!inst->group_started || !inst->kanban_started || group_protocol_current(inst, group) != GROUP_PROTOCOL_KANBAN) return -1;
    struct kb_sql_scope* scope = u_calloc(1, sizeof(*scope));
    if (!scope) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board SQL snapshot allocation ch=%s", group); return -1; }
    memcpy(scope->self, inst->my_ed25519_pubkey, 32); scope->generation = inst->kanban_generation; *out = scope; return 0;
}
static int kb_sql_work(sqlite3* db, void* data, struct merkle_sync_sql_request* request) {
    struct kb_sql_scope* scope = data; int code = SQLITE_OK;
    switch (request->action) {
    case MT_SQL_PAGE:
        request->length = request->capacity;
        code = kb_page_read(db, request->ns, request->prefix, request->after_valid, &request->after,
            request->data, &request->length, &request->next, &request->more); break;
    case MT_SQL_BEGIN_PULL: code = kb_begin_write(db, request->ns, &request->pull); break;
    case MT_SQL_STAGE_PAGE: code = kb_stage_write(db, request->pull, request->prefix, request->after_valid,
        &request->after, &request->next, request->more, request->data, request->length); break;
    case MT_SQL_COMMIT_PULL: code = kb_commit_write(db, scope->self, request->pull); break;
    case MT_SQL_ABORT_PULL: code = kb_abort_write(db, request->pull); break;
    case MT_SQL_FINISH: break;
    default: code = -SQLITE_MISUSE; break;
    }
    if (code < 0) code = -code;
    if (code && (request->action == MT_SQL_PAGE || request->action == MT_SQL_BEGIN_PULL)) request->result = MT_ERR_IO;
    else request->result = code ? (code == SQLITE_AUTH || code == SQLITE_MISMATCH ? MT_ERR_DATA : MT_ERR_IO) : MT_OK;
    return code;
}
static void kb_sql_publish(void* ctx, void* data, struct merkle_sync_sql_request* request) {
    struct UTUN_INSTANCE* inst = ctx; struct kb_sql_scope* scope = data;
    if (request->action == MT_SQL_COMMIT_PULL && inst->group_started && inst->kanban_started &&
        inst->kanban_generation == scope->generation && ((struct kb_pull*)request->pull)->changed) kb_publish(inst, request->ns);
}
static void kb_sql_free(void* scope) { u_free(scope); }
static void kb_sql_pull_free(void* pull) { u_free(pull); }
static const struct merkle_sync_sql_ops kb_sql_ops = {.snapshot=kb_sql_snapshot, .work=kb_sql_work,
    .publish=kb_sql_publish, .free=kb_sql_free, .pull_free=kb_sql_pull_free};
static const struct merkle_sync_data_ops ops = {
    .group_scoped = 1,
    .update_bucket_hash = leaf, .get_page = page, .validate_peer = eligible, .on_result = result,
    .begin_pull = begin, .stage_page = stage, .commit_pull = commit, .publish_pull = publish, .abort_pull = abort_pull, .sql=&kb_sql_ops
};
int kb_sync_init(struct UTUN_INSTANCE* inst) { return merkle_sync_register(inst, MT_DATA_KANBAN, &ops, inst); }
void kb_sync_stop(struct UTUN_INSTANCE* inst) { merkle_sync_unregister(inst, MT_DATA_KANBAN); }
void kanban_peer_ready(struct UTUN_INSTANCE* inst, const char* group, const uint8_t peer[32]) {
    struct ETCP_CONN* conn = instance_find_conn_by_ed(inst, peer);
    if (!eligible(inst, group, conn)) return;
    if (merkle_sync_start(inst, MT_DATA_KANBAN, conn, group, NULL, NULL) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "board sync unavailable ch=%s peer=%s", group, sc_public_key_hex(peer, (char[65]){0}));
}
