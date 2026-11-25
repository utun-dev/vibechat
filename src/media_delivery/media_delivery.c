#include "media_delivery.h"
#include "../db_schema.h"
#include "media_delivery_proto.h"
#include "../attachments/store.h"
#include "media_download.h"
#include "media_index.h"
#include "../../lib/file_io.h"
#include "../utun_instance.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/route_crypto.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/conn_mgr.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/secure_channel.h"
#include "../chat/member_sync.h"
#include "../chat/group_policy.h"
#include "../chat/chat_core.h"
#include "../../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/json_flat.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../lib/sqlite3.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <openssl/crypto.h>
#include "../lib/platform_compat.h"

#define MD_ID "media_delivery"

/* ── forward decls ── */

static void md_block_wake(struct media_block_source* source, struct stream_ctx* executing);
static void md_etcp_recv(struct ETCP_CONN* conn, struct ll_entry* entry);
static void md_on_conn_status(struct ETCP_CONN* conn, int status, void* arg);
static void md_on_props_changed(const uint8_t ed[32], const char* channel_id, void* arg);
static void md_super_connect(struct media_delivery_ctx* md, const uint8_t ed[32], uint64_t group_id);
static void md_repl_pump(void* arg);
static void md_repl_watchdog(void* arg);
static void md_repl_notify(struct media_delivery_ctx* md, uint64_t group_id);
static int md_group_role(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t ed[32]);
static int md_collect_supernodes(struct UTUN_INSTANCE* inst, uint64_t group_id, uint8_t supers[10][32]);
static int md_repl_request_submit(struct media_super_peer* peer);
static int md_repl_page_submit(struct media_super_peer* peer);
static int md_repl_cursor_read(sqlite3* db, const char* channel, const uint8_t remote[32], struct media_pkt_sync_req* request);
static uint64_t md_read_le(const void* ptr, unsigned bytes);
static void md_write_le(void* ptr, uint64_t value, unsigned bytes);
static void md_super_conn_cb(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t group_id,
                             enum conn_mgr_event event, void* arg);

/* ── helpers ── */

static const char* md_subcmd_name(uint8_t sc) {
    switch (sc) {
    case MEDIA_SUBCMD_SERVE_REG:      return "SERVE_REG";
    case MEDIA_SUBCMD_SERVE_ACK:      return "SERVE_ACK";
    case MEDIA_SUBCMD_SERVE_LEAVE:    return "SERVE_LEAVE";
    case MEDIA_SUBCMD_QUERY:          return "QUERY";
    case MEDIA_SUBCMD_QUERY_RESP:     return "QUERY_RESP";
    case MEDIA_SUBCMD_HAVE_BLOCK:     return "HAVE_BLOCK";
    case MEDIA_SUBCMD_HAVE_BLOCK_ACK: return "HAVE_BLOCK_ACK";
    case MEDIA_SUBCMD_SYNC_REQ:       return "SYNC_REQ";
    case MEDIA_SUBCMD_SYNC_RESP:      return "SYNC_RESP";
    case MEDIA_SUBCMD_BLOCK_REQ:      return "BLOCK_REQ";
    case MEDIA_SUBCMD_BLOCK_CHUNK:    return "BLOCK_CHUNK";
    case MEDIA_SUBCMD_BLOCK_DONE:     return "BLOCK_DONE";
    case MEDIA_SUBCMD_CANCEL:         return "CANCEL";
    case MEDIA_SUBCMD_CHANGED:        return "CHANGED";
    case MEDIA_SUBCMD_BLOCK_PROCESSING: return "BLOCK_PROCESSING";
    case MEDIA_SUBCMD_BLOCK_RELAY_FULL: return "BLOCK_RELAY_FULL";
    default:                          return "???";
    }
}

/* Все новые SQL-пути проверяют и завершение statement: ошибка finalize не является успехом. */
static int md_sql_result(sqlite3* db, sqlite3_stmt* st, int rc, const char* phase) {
    int final_rc = sqlite3_finalize(st);
    if (rc == SQLITE_DONE && final_rc == SQLITE_OK) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: SQL phase=%s rc=%d finalize=%d error=%s",
                MD_ID, phase, rc, final_rc, sqlite3_errmsg(db));
    return rc != SQLITE_DONE && rc != SQLITE_OK ? rc : final_rc != SQLITE_OK ? final_rc : SQLITE_ERROR;
}
static int md_sql_finish(sqlite3* db, sqlite3_stmt* st, int rc, const char* phase) {
    return md_sql_result(db, st, rc, phase) ? -1 : 0;
}

static int md_sql_exec(sqlite3* db, const char* sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: SQL rc=%d command=%s error=%s", MD_ID, rc, sql, sqlite3_errmsg(db));
    return -1;
}

/* Чужая транзакция не даёт гарантии commit; её владелец должен завершить работу до репликации. */
static int md_sql_begin(sqlite3* db, const char* sql) {
    if (!db || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot own transaction db=%p", MD_ID, (void*)db); return -1;
    }
    return md_sql_exec(db, sql);
}

/* SQLite может сама откатить транзакцию при отказе commit/IO; чужую или уже завершённую не трогаем. */
static void md_sql_rollback(sqlite3* db) {
    if (!sqlite3_get_autocommit(db)) md_sql_exec(db, "ROLLBACK");
}

/* ID посредника не подписывается. Все прочие поля wire фиксированы и имеют LE-порядок. */
static const uint8_t* md_group_key(struct UTUN_INSTANCE* inst, uint64_t group) {
    return topo_group_wire_id(inst->topo_groups ? topo_groups_find(inst->topo_groups, group) : NULL);
}
static int md_holder_body(const uint8_t group_ed[32], const struct media_pkt_sync_entry* row, uint8_t body[sizeof(*row)]) {
    static const char domain[] = "utun-media-holder-v3";
    size_t fields = offsetof(struct media_pkt_sync_entry, signature) - offsetof(struct media_pkt_sync_entry, block_id);
    if (!group_ed || !row || !memcmp(row->holder, (const uint8_t[32]){0}, sizeof(row->holder))
        || md_read_le(&row->timestamp, 8) > INT64_MAX || !md_read_le(&row->version, 8)
        || md_read_le(&row->version, 8) > INT64_MAX || row->status > 1) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: invalid holder record group=%s", MD_ID, sc_public_key_hex(group_ed, (char[65]){0})); return -1;
    }
    memcpy(body, domain, sizeof(domain) - 1); memcpy(body + sizeof(domain) - 1, group_ed, 32);
    memcpy(body + sizeof(domain) - 1 + 32, row->block_id, fields);
    return (int)(sizeof(domain) - 1 + 32 + fields);
}

int media_delivery_sign_holder(const uint8_t group_ed[32], struct media_pkt_sync_entry* row, const uint8_t private_key[32]) {
    uint8_t body[sizeof(*row)], signature[64]; int size = md_holder_body(group_ed, row, body);
    if (!private_key || size < 0 || sc_ed25519_sign(private_key, body, size, signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: holder signing failed group=%s", MD_ID, sc_public_key_hex(group_ed, (char[65]){0})); return -1;
    }
    memcpy(row->signature, signature, 64); return 0;
}

static int md_holder_verify(struct media_delivery_ctx* md, uint64_t group, const struct media_pkt_sync_entry* row) {
    uint8_t body[sizeof(*row)]; int size = md_holder_body(md_group_key(md->inst, group), row, body);
    const char* channel = topo_groups_channel(md->inst->topo_groups, group);
    int known = size >= 0 && group_policy_member_verified(md->inst, channel, row->holder);
    int rc = known ? sc_ed25519_verify(row->holder, body, size, row->signature) : SC_ERR_NOT_INITIALIZED;
    if (rc != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: holder record rejected group=%llx holder=%s verified=%d signature_rc=%d",
            MD_ID, (unsigned long long)group, sc_public_key_hex(row ? row->holder : NULL, (char[65]){0}), known, rc); return -1;
    }
    return 0;
}

/* Только проверенная строка, внутри транзакции. Чужой chunk не блокирует другого держателя. */
static int md_ba_merge(sqlite3* db, const char* channel, const struct media_pkt_sync_entry* row) {
    uint64_t version = md_read_le(&row->version, 8);
    uint32_t chunk = (uint32_t)md_read_le(&row->chunk, 4); int64_t ts = (int64_t)md_read_le(&row->timestamp, 8);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT chunk,timestamp,status,version FROM block_availability WHERE group_id=? AND block_uuid=? AND holder=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, row->block_id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, row->holder, sizeof(row->holder), SQLITE_STATIC);
    int unchanged = 0;
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int old_status = sqlite3_column_int(st, 2); uint64_t old_version = (uint64_t)sqlite3_column_int64(st, 3);
        if (old_status > row->status || (old_status == row->status && old_version > version)) unchanged = 1;
        if (old_status == row->status && old_version == version) {
            struct media_pkt_sync_entry previous = *row;
            md_write_le(&previous.chunk, (uint64_t)sqlite3_column_int64(st, 0), 4);
            md_write_le(&previous.timestamp, (uint64_t)sqlite3_column_int64(st, 1), 8);
            size_t fields = offsetof(struct media_pkt_sync_entry, signature) - offsetof(struct media_pkt_sync_entry, block_id);
            int order = memcmp(row->block_id, previous.block_id, fields);
            if (order) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: holder equivocation group=%s holder=%s version=%llu incoming_wins=%d",
                MD_ID, channel, sc_public_key_hex(row->holder, (char[65]){0}), (unsigned long long)version, order > 0);
            unchanged = order <= 0;
        }
    }
    rc = md_sql_result(db, st, rc, "merge lookup");
    if (rc) return -rc;
    if (unchanged) return 0;
    st = NULL;
    rc = sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO block_availability"
        "(block_uuid,group_id,holder,chunk,timestamp,status,version,signature) VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, row->block_id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, row->holder, sizeof(row->holder), SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 4, chunk);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 5, ts);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 6, row->status);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 7, (sqlite3_int64)version);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 8, row->signature, 64, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    rc = md_sql_result(db, st, rc, "merge insert");
    return rc ? -rc : 1;
}

/* Собственная запись подписывается и сохраняется до отправки; caller держит транзакцию. */
static int md_holder_own_write(sqlite3* db, const uint8_t self[32], const uint8_t private_key[32],
    uint64_t group, const char* channel, const uint8_t media[16], const uint8_t block[16], uint32_t chunk,
    int64_t timestamp, int status, struct media_pkt_sync_entry* out) {
    int published = attachment_store_public_read(db, channel, media);
    if (published <= 0) {
        if (!published) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: holder suppressed before publication group=%llx media=%02x%02x chunk=%u",
            MD_ID, (unsigned long long)group, media[0], media[1], chunk);
        return published < 0 ? published : -SQLITE_NOTFOUND;
    }
    struct media_pkt_sync_entry row = {0}; memcpy(row.block_id, block, 16);
    memcpy(row.holder, self, sizeof(row.holder)); md_write_le(&row.chunk, chunk, 4);
    md_write_le(&row.timestamp, (uint64_t)timestamp, 8); row.status = status;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT chunk,timestamp,status,version,signature FROM block_availability"
        " WHERE group_id=? AND block_uuid=? AND holder=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, block, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, self, sizeof(row.holder), SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    uint64_t previous = 0; int reuse = 0;
    if (rc == SQLITE_ROW) {
        previous = (uint64_t)sqlite3_column_int64(st, 3);
        if (!previous || previous > INT64_MAX || sqlite3_column_bytes(st, 4) != 64) rc = SQLITE_CORRUPT;
        else {
            int old_status = sqlite3_column_int(st, 2);
            if (old_status > row.status) row.status = old_status;
            reuse = (uint32_t)sqlite3_column_int64(st, 0) == chunk && old_status >= status;
            if (reuse) {
                row.status = old_status; md_write_le(&row.timestamp, (uint64_t)sqlite3_column_int64(st, 1), 8);
                md_write_le(&row.version, previous, 8); memcpy(row.signature, sqlite3_column_blob(st, 4), 64);
            }
            rc = sqlite3_step(st);
        }
    }
    rc = md_sql_result(db, st, rc, "read own holder");
    if (rc) return -rc;
    if (reuse) { *out = row; return 0; }
    if (previous == INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: own holder version exhausted group=%llx", MD_ID, (unsigned long long)group);
        return -SQLITE_FULL;
    }
    md_write_le(&row.version, previous + 1, 8);
    uint8_t group_ed[32]; int found = 0;
    rc = member_sync_channel_keys_read(db, channel, NULL, group_ed, &found);
    if (rc || !found) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: holder group key unavailable group=%s rc=%d", MD_ID, channel, rc);
        return rc ? -rc : -SQLITE_AUTH;
    }
    if (media_delivery_sign_holder(group_ed, &row, private_key)) return -SQLITE_ERROR;
    int changed = md_ba_merge(db, channel, &row);
    if (changed >= 0) {
        *out = row;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: own holder signed group=%llx holder=%s chunk=%u status=%u version=%llu changed=%d",
            MD_ID, (unsigned long long)group, sc_public_key_hex(self, (char[65]){0}), chunk, row.status,
            (unsigned long long)(previous + 1), changed);
    }
    return changed;
}
static int md_holder_own(struct media_delivery_ctx* md, uint64_t group, const uint8_t media[16], const uint8_t block[16], uint32_t chunk,
    int64_t timestamp, int status, struct media_pkt_sync_entry* out) {
    return md_holder_own_write(md->db, md->inst->my_ed25519_pubkey, md->inst->my_ed25519_privkey, group, topo_groups_channel(md->inst->topo_groups, group), media, block, chunk, timestamp, status, out);
}

/* SQL-входы лежат после job и не содержат runtime. Список принадлежит только uasync. */
struct md_sql_job {
    struct md_sql_job* next;
    struct media_delivery_ctx* owner;
    struct media_super_peer* peer; /* Только uasync; remove/quiesce обнуляет до освобождения peer. */
    struct UTUN_INSTANCE* inst;
    uint64_t generation, group;
    const char* name;
    enum db_async_mode mode;
    db_async_work_fn work;
    void (*done)(struct md_sql_job*, const struct db_async_result*);
    int (*guard)(struct md_sql_job*);
    void (*cleanup)(void*);
    int retry, peer_required, cancelled;
};
static void md_sql_dispatch(struct md_sql_job* job);
static int md_sql_current(void* arg) {
    struct md_sql_job* job = arg;
    return job->owner && !job->cancelled && !job->inst->destroying &&
        job->owner == &job->inst->md && job->owner->initialized && job->inst->group_generation == job->generation &&
        (!job->peer_required || job->peer);
}
static void md_sql_unlink(struct md_sql_job* job) {
    if (!job->owner) return;
    struct md_sql_job** slot = &job->owner->sql_jobs;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot) *slot = job->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: SQL owner lost job=%s", MD_ID, job->name);
    if (job->peer) {
        if (job->peer->request_sql == job) job->peer->request_sql = NULL;
        if (job->peer->page_sql == job) job->peer->page_sql = NULL;
        if (job->peer->merge_sql == job) job->peer->merge_sql = NULL;
    }
    job->owner = NULL; job->next = NULL;
}
static void md_sql_free(struct md_sql_job* job) {
    md_sql_unlink(job); if (job->cleanup) job->cleanup(job + 1); u_free(job);
}
static struct md_sql_job* md_sql_new(struct media_delivery_ctx* md, uint64_t group, size_t size,
    const char* name, enum db_async_mode mode, db_async_work_fn work,
    void (*done)(struct md_sql_job*, const struct db_async_result*)) {
    if (!md || !md->initialized || md->inst->destroying || size > UINT32_MAX - sizeof(struct md_sql_job)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: SQL rejected before submit name=%s", MD_ID, name); return NULL;
    }
    unsigned pending = 0;
    for (struct md_sql_job* active = md->sql_jobs; active; active = active->next) pending++;
    if (pending >= 256) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: owned SQL queue full name=%s pending=%u", MD_ID, name, pending); return NULL; }
    struct md_sql_job* job = u_calloc(1, sizeof(*job) + size);
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: SQL allocation failed name=%s", MD_ID, name); return NULL; }
    job->owner = md; job->inst = md->inst; job->group = group; job->generation = md->inst->group_generation;
    job->name = name; job->mode = mode; job->work = work; job->done = done;
    job->next = md->sql_jobs; md->sql_jobs = job; return job;
}
static int md_sql_guard(void* arg) {
    struct md_sql_job* job = arg; return md_sql_current(job) && (!job->guard || job->guard(job));
}
static int md_sql_work(sqlite3* db, void* arg) {
    struct md_sql_job* job = arg; return job->work(db, job + 1);
}
static int md_sql_transient(const struct db_async_result* result) {
    int code = result->sqlite_code & 255;
    return result->status == DB_ASYNC_FULL || result->status == DB_ASYNC_NOMEM ||
        (result->status == DB_ASYNC_SQL && (code == SQLITE_BUSY || code == SQLITE_LOCKED || code == SQLITE_NOMEM || code == SQLITE_FULL));
}
static void md_sql_done(void* arg, const struct db_async_result* result) {
    struct md_sql_job* job = arg;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: SQL name=%s group=%llx status=%d rc=%d current=%d: %s",
        MD_ID, job->name, (unsigned long long)job->group, result->status, result->sqlite_code, md_sql_current(job), result->message);
    if (md_sql_current(job) && md_sql_transient(result)) { job->retry = 1; return; }
    int current = md_sql_current(job); md_sql_unlink(job);
    if (!current && !result->status) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: discard late SQL completion name=%s group=%llx", MD_ID,
        job->name, (unsigned long long)job->group);
    if (current && job->done) job->done(job, result);
    md_sql_free(job);
}
static void md_sql_dispatch(struct md_sql_job* job) {
    job->retry = 0;
    db_async_submit_guarded(job->inst->db_worker, job->name, job->mode, md_sql_guard, md_sql_work, job, md_sql_done);
}
static void md_sql_retry(struct media_delivery_ctx* md) {
    for (struct md_sql_job* job = md->sql_jobs; job; job = job->next)
        if (job->retry) { md_sql_dispatch(job); return; }
}
static void md_sql_detach(struct media_delivery_ctx* md) {
    while (md->sql_jobs) {
        struct md_sql_job* job = md->sql_jobs; md_sql_unlink(job); job->peer = NULL;
        if (job->retry) md_sql_free(job);
    }
}
static void md_sql_peer_detach(struct media_delivery_ctx* md, struct media_super_peer* peer) {
    for (struct md_sql_job* job = md->sql_jobs, *next; job; job = next) {
        next = job->next;
        if (job->peer != peer) continue;
        job->peer = NULL; if (job->retry) md_sql_free(job);
    }
    peer->request_sql = peer->page_sql = peer->merge_sql = NULL;
}

struct md_role_sql {
    char channel[65];
    uint64_t group;
    uint8_t self_ed[32], private_key[32];
    int all, seed, changed;
};
static int md_role_write(sqlite3* db, void* arg) {
    struct md_role_sql* data = arg; sqlite3_stmt* st = NULL; data->changed = 0;
    if (!data->seed) {
        int rc = sqlite3_prepare_v2(db, "DELETE FROM block_availability WHERE group_id=? AND (? OR holder!=?)", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, data->channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int(st, 2, data->all);
        if (!rc) rc = sqlite3_bind_blob(st, 3, data->self_ed, sizeof(data->self_ed), SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        rc = md_sql_result(db, st, rc, "purge group rows"); if (rc) return rc;
        data->changed = sqlite3_changes(db); st = NULL;
        rc = sqlite3_prepare_v2(db, "DELETE FROM super_sync WHERE group_id=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, data->channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        return md_sql_result(db, st, rc, "purge group cursors");
    }
    const char* channel = data->channel;
    int rc = sqlite3_prepare_v2(db, "SELECT block_id,chunk,timestamp,media_id FROM media_public_files WHERE chat_id=? AND holder=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->self_ed, 32, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 chunk = sqlite3_column_int64(st, 1), timestamp = sqlite3_column_int64(st, 2);
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 16 ||
            sqlite3_column_type(st, 1) != SQLITE_INTEGER || chunk < 0 || chunk > UINT32_MAX || timestamp < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: malformed own block group=%llx", MD_ID, (unsigned long long)data->group);
            rc = SQLITE_CORRUPT; break;
        }
        struct media_pkt_sync_entry row;
        int applied = md_holder_own_write(db, data->self_ed, data->private_key, data->group, channel,
            sqlite3_column_blob(st, 3), sqlite3_column_blob(st, 0), (uint32_t)chunk, timestamp, 1, &row);
        if (applied < 0) { rc = -applied; break; }
        data->changed += applied;
    }
    return md_sql_result(db, st, rc, "seed own blocks");
}
static void md_role_done(struct md_sql_job* job, const struct db_async_result* result) {
    struct md_role_sql* data = (void*)(job + 1);
    if (result->status || !data->changed) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: role catalog committed group=%llx seed=%d all=%d changed=%d", MD_ID,
        (unsigned long long)data->group, data->seed, data->all, data->changed);
    if (data->seed) md_repl_notify(&job->inst->md, data->group);
}
static void md_role_cleanup(void* arg) {
    struct md_role_sql* data = arg; OPENSSL_cleanse(data->private_key, sizeof(data->private_key));
}
static int md_role_guard(struct md_sql_job* job) {
    struct md_role_sql* data = (void*)(job + 1);
    return !data->seed || topo_group_chat_key_allowed(job->inst, data->group, data->self_ed);
}
static int md_role_submit(struct media_delivery_ctx* md, uint64_t group, int seed, int all) {
    for (struct md_sql_job* job = md->sql_jobs; job; job = job->next) {
        if (job->work != md_role_write || job->group != group) continue;
        struct md_role_sql* data = (void*)(job + 1);
        if (data->seed == seed && data->all == all) return 0;
    }
    struct md_sql_job* job = md_sql_new(md, group, sizeof(struct md_role_sql), seed ? "media seed" : "media purge",
        DB_ASYNC_WRITE, md_role_write, md_role_done);
    if (!job) return -1;
    struct md_role_sql* data = (void*)(job + 1);
    strcpy(data->channel, topo_groups_channel(md->inst->topo_groups, group));
    data->group = group; data->seed = seed; data->all = all;
    memcpy(data->self_ed, md->inst->my_ed25519_pubkey, sizeof(data->self_ed));
    memcpy(data->private_key, md->inst->my_ed25519_privkey, 32);
    job->guard = md_role_guard; job->cleanup = md_role_cleanup; md_sql_dispatch(job); return 0;
}

struct md_announce_sql {
    char channel[65];
    uint64_t group;
    uint8_t self_ed[32], peer[32], private_key[32];
    struct media_pkt_have_block packet;
    struct media_pkt_sync_entry row;
    int incoming, status, changed, suppressed;
};
static int md_announce_write(sqlite3* db, void* arg) {
    struct md_announce_sql* data = arg; data->changed = 0;
    if (data->incoming) {
        int applied = md_ba_merge(db, data->channel, &data->row);
        if (applied < 0) return -applied;
        data->changed = applied; return SQLITE_OK;
    }
    sqlite3_stmt* st = NULL; const char* channel = data->channel;
    int rc = sqlite3_prepare_v2(db, "SELECT timestamp FROM media_public_files WHERE chat_id=? AND media_id=? AND block_id=? AND holder=? AND chunk=?",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->packet.media_id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, data->packet.block_id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, data->self_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 5, data->packet.chunk);
    if (!rc) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) < 0) rc = SQLITE_CORRUPT;
            else { data->packet.timestamp = sqlite3_column_int64(st, 0); rc = sqlite3_step(st); }
        }
    }
    rc = md_sql_result(db, st, rc, "read own announcement timestamp"); if (rc) return rc;
    int changed = md_holder_own_write(db, data->self_ed, data->private_key, data->group, channel, data->packet.media_id, data->packet.block_id,
        data->packet.chunk, data->packet.timestamp, data->status, &data->row);
    if (changed == -SQLITE_NOTFOUND) { data->suppressed = 1; return SQLITE_OK; }
    if (changed < 0) return -changed;
    data->changed = changed; return SQLITE_OK;
}
static int md_announce_guard(struct md_sql_job* job) {
    struct md_announce_sql* data = (void*)(job + 1);
    const char* channel = data->channel;
    return topo_group_chat_key_allowed(job->inst, data->group, data->peer) &&
        (!data->incoming || (group_policy_member_role_key(job->inst, channel, data->self_ed, GROUP_MEMBER_SUPERNODE) == 1 &&
            !md_holder_verify(&job->inst->md, data->group, &data->row)));
}
static void md_announce_done(struct md_sql_job* job, const struct db_async_result* result) {
    struct md_announce_sql* data = (void*)(job + 1);
    if (data->suppressed || !md_announce_guard(job)) return;
    if (data->incoming && data->status) {
        struct media_pkt_have_block_ack ack = {.subcmd=MEDIA_SUBCMD_HAVE_BLOCK_ACK,
            .status=result->status ? 2 : data->changed ? 0 : 1};
        memcpy(ack.media_id, data->packet.media_id, 16); memcpy(ack.block_id, data->packet.block_id, 16);
        md_send_key(job->inst, data->group, data->peer, (const uint8_t*)&ack, sizeof(ack), 1);
    } else if (!data->incoming && !result->status) {
        data->packet.subcmd = data->row.status ? MEDIA_SUBCMD_HAVE_BLOCK : MEDIA_SUBCMD_BLOCK_PROCESSING;
        data->packet.chunk = (uint32_t)md_read_le(&data->row.chunk, 4);
        data->packet.timestamp = (int64_t)md_read_le(&data->row.timestamp, 8);
        memcpy(&data->packet.version, &data->row.version, 8); memcpy(data->packet.signature, data->row.signature, 64);
        md_send_key(job->inst, data->group, data->peer, (const uint8_t*)&data->packet, sizeof(data->packet), 1);
    }
    if (!result->status && data->changed) md_repl_notify(&job->inst->md, data->group);
}
static void md_announce_cleanup(void* arg) {
    struct md_announce_sql* data = arg; OPENSSL_cleanse(data->private_key, sizeof(data->private_key));
}
static void md_announce_submit(struct media_delivery_ctx* md, uint64_t group, const uint8_t peer[32],
    const struct media_pkt_have_block* packet, const struct media_pkt_sync_entry* incoming, int status) {
    struct md_sql_job* job = md_sql_new(md, group, sizeof(struct md_announce_sql), "media announcement", DB_ASYNC_WRITE,
        md_announce_write, md_announce_done);
    if (!job) return;
    struct md_announce_sql* data = (void*)(job + 1); data->group = group;
    strcpy(data->channel, topo_groups_channel(md->inst->topo_groups, group));
    memcpy(data->self_ed, md->inst->my_ed25519_pubkey, sizeof(data->self_ed));
    memcpy(data->peer, peer, sizeof(data->peer)); data->packet = *packet; data->incoming = incoming != NULL; data->status = status;
    if (incoming) data->row = *incoming;
    else memcpy(data->private_key, md->inst->my_ed25519_privkey, 32);
    job->guard = md_announce_guard; job->cleanup = md_announce_cleanup; md_sql_dispatch(job);
}

struct md_local_block { char channel[65]; uint8_t media[16], block[16]; uint32_t chunk; };
struct md_local_sql { uint8_t self_ed[32]; size_t count, capacity; struct md_local_block* rows; };
static int md_local_read(sqlite3* db, void* arg) {
    struct md_local_sql* data = arg; data->count = 0; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT id,media_id,block_id,chunk,chat_id FROM media_public_files WHERE holder=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->self_ed, 32, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 chunk = sqlite3_column_int64(st, 3); const char* channel = (const char*)sqlite3_column_text(st, 4);
        int bytes = sqlite3_column_bytes(st, 4);
        if (sqlite3_column_type(st, 4) != SQLITE_TEXT || bytes != 64 || !chat_channel_valid(channel) || sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 16 ||
            sqlite3_column_type(st, 2) != SQLITE_BLOB || sqlite3_column_bytes(st, 2) != 16 ||
            sqlite3_column_type(st, 3) != SQLITE_INTEGER || chunk < 0 || chunk > UINT32_MAX) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: local announcement skipped malformed row=%lld", MD_ID,
                (long long)sqlite3_column_int64(st, 0)); continue;
        }
        if (data->count == data->capacity) {
            size_t next = data->capacity ? data->capacity * 2 : 16;
            if (next > UINT32_MAX / sizeof(*data->rows)) { rc = SQLITE_TOOBIG; break; }
            struct md_local_block* rows = u_realloc(data->rows, next * sizeof(*rows));
            if (!rows) { rc = SQLITE_NOMEM; break; }
            data->rows = rows; data->capacity = next;
        }
        struct md_local_block* row = &data->rows[data->count++]; strcpy(row->channel, channel); row->chunk = (uint32_t)chunk;
        memcpy(row->media, sqlite3_column_blob(st, 1), 16); memcpy(row->block, sqlite3_column_blob(st, 2), 16);
    }
    return md_sql_result(db, st, rc, "local announcement snapshot");
}
static void md_local_done(struct md_sql_job* job, const struct db_async_result* result) {
    struct md_local_sql* data = (void*)(job + 1); if (result->status) return;
    for (size_t i = 0; i < data->count; i++) {
        const struct md_local_block* row = &data->rows[i]; uint8_t supers[10][32];
        uint64_t group = topo_groups_channel_slot(job->inst->topo_groups, row->channel);
        if (!topo_group_chat_key_allowed(job->inst, group, data->self_ed)) continue;
        int count = md_collect_supernodes(job->inst, group, supers);
        for (int n = 0; n < count; n++) media_delivery_send_have_block(job->inst, group, supers[n], row->media, row->block, row->chunk);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: local announcement snapshot delivered rows=%zu", MD_ID, data->count);
}
static void md_local_cleanup(void* arg) { u_free(((struct md_local_sql*)arg)->rows); }
static void md_local_submit(struct media_delivery_ctx* md) {
    for (struct md_sql_job* job = md->sql_jobs; job; job = job->next) if (job->work == md_local_read) return;
    struct md_sql_job* job = md_sql_new(md, 0, sizeof(struct md_local_sql), "media local blocks", DB_ASYNC_READ, md_local_read, md_local_done);
    if (!job) return;
    struct md_local_sql* data = (void*)(job + 1);
    memcpy(data->self_ed, md->inst->my_ed25519_pubkey, 32); job->cleanup = md_local_cleanup; md_sql_dispatch(job);
}

/* Выбрать ограниченный список кандидатов: свой готовый блок, затем свежие HAVE/PROCESSING.
 * Исключения применяются до LIMIT; processing сохраняется для промежуточной раздачи. */
int md_query_holders_read(sqlite3* db, const uint8_t self_ed[32], const char* channel, const uint8_t requester[32],
                            const uint8_t* media_id, struct media_pkt_query_resp_entry* result) {
    result->num_holders = 0; memset(result->holders, 0, sizeof(result->holders));
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT holder FROM ("
        " SELECT holder,0 AS own,timestamp FROM block_availability WHERE block_uuid=?1 AND group_id=?2"
        " UNION ALL SELECT holder,1 AS own,timestamp FROM media_public_files WHERE block_id=?1 AND media_id=?3 AND holder=?4 AND chat_id=?5"
        ") AS candidates WHERE holder<>zeroblob(32) AND holder!=?6 AND EXISTS(SELECT 1 FROM peers_%s p"
        " WHERE p.ed25519_pubkey=candidates.holder AND p.record_state=0 AND p.source=0 AND p.identity_verified=1 AND p.admitted=1 AND p.deleted=0)"
        " GROUP BY holder ORDER BY MAX(own) DESC,MAX(timestamp) DESC,holder ASC LIMIT ?7", channel);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, result->block_id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, media_id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, self_ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 5, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 6, requester, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 7, MEDIA_QUERY_MAX_HOLDERS);
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (result->num_holders >= MEDIA_QUERY_MAX_HOLDERS || sqlite3_column_type(st, 0) != SQLITE_BLOB ||
                sqlite3_column_bytes(st, 0) != 32) { rc = SQLITE_CORRUPT; break; }
            memcpy(result->holders[result->num_holders++], sqlite3_column_blob(st, 0), sizeof(result->holders[0]));
        }
    }
    int final_rc = sqlite3_finalize(st);
    if (rc == SQLITE_DONE && final_rc == SQLITE_OK) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: QUERY SQL failed rc=%d finalize=%d: %s", MD_ID, rc, final_rc, sqlite3_errmsg(db));
    return rc != SQLITE_DONE ? rc : final_rc;
}
struct md_query_sql {
    char channel[65];
    uint64_t group;
    uint8_t self_ed[32], peer[32];
    struct media_pkt_query request;
    uint8_t blocks[MEDIA_QUERY_MAX_BLOCKS][16];
    uint8_t response[MEDIA_QUERY_RESP_HDR_SIZE + MEDIA_QUERY_MAX_BLOCKS * sizeof(struct media_pkt_query_resp_entry)];
};
static void md_query_init(struct md_query_sql* data) {
    memset(data->response, 0, sizeof(data->response));
    struct media_pkt_query_resp* response = (void*)data->response; response->subcmd = MEDIA_SUBCMD_QUERY_RESP;
    memcpy(response->media_id, data->request.media_id, 16); memcpy(response->request_id, data->request.request_id, 16);
    response->num_blocks = data->request.num_blocks;
}
static int md_query_read(sqlite3* db, void* arg) {
    struct md_query_sql* data = arg; md_query_init(data);
    struct media_pkt_query_resp_entry* rows = (void*)(data->response + MEDIA_QUERY_RESP_HDR_SIZE);
    for (unsigned i = 0; i < data->request.num_blocks; i++) {
        memcpy(rows[i].block_id, data->blocks[i], 16);
        int rc = md_query_holders_read(db, data->self_ed, data->channel, data->peer, data->request.media_id, rows + i);
        if (rc) return rc;
    }
    return SQLITE_OK;
}
static int md_query_guard(struct md_sql_job* job) {
    struct md_query_sql* data = (void*)(job + 1);
    return topo_group_chat_key_allowed(job->inst, data->group, data->peer);
}
static void md_query_done(struct md_sql_job* job, const struct db_async_result* result) {
    struct md_query_sql* data = (void*)(job + 1); if (!md_query_guard(job)) return;
    struct media_pkt_query_resp* response = (void*)data->response;
    if (result->status) { response->status = MEDIA_QUERY_DB_ERROR; response->num_blocks = 0; }
    else {
        struct media_pkt_query_resp_entry* rows = (void*)(data->response + MEDIA_QUERY_RESP_HDR_SIZE);
        for (unsigned i = 0; i < response->num_blocks; i++) {
            unsigned count = rows[i].num_holders, kept = 0;
            for (unsigned n = 0; n < count; n++) if (topo_group_chat_key_allowed(job->inst, data->group, rows[i].holders[n])) {
                if (kept != n) memcpy(rows[i].holders[kept], rows[i].holders[n], sizeof(rows[i].holders[0]));
                kept++;
            }
            memset(rows[i].holders + kept, 0, (MEDIA_QUERY_MAX_HOLDERS - kept) * sizeof(rows[i].holders[0]));
            rows[i].num_holders = (uint8_t)kept;
        }
    }
    md_send_key(job->inst, data->group, data->peer, data->response,
        MEDIA_QUERY_RESP_HDR_SIZE + response->num_blocks * sizeof(struct media_pkt_query_resp_entry), 1);
}
static void md_query_submit(struct media_delivery_ctx* md, uint64_t group, const uint8_t peer[32], const struct media_pkt_query* request) {
    struct md_sql_job* job = md_sql_new(md, group, sizeof(struct md_query_sql), "media query", DB_ASYNC_READ, md_query_read, md_query_done);
    if (!job) return;
    struct md_query_sql* data = (void*)(job + 1); data->group = group; strcpy(data->channel, topo_groups_channel(job->inst->topo_groups, data->group));
    memcpy(data->self_ed, md->inst->my_ed25519_pubkey, sizeof(data->self_ed)); memcpy(data->peer, peer, sizeof(data->peer));
    data->request = *request; memcpy(data->blocks, (const uint8_t*)request + MEDIA_QUERY_HDR_SIZE, request->num_blocks * 16);
    md_query_init(data); job->guard = md_query_guard; md_sql_dispatch(job);
}

/* ── super_peer find/create ── */

/* Обе очереди индексируются по group_id и Ed25519, как и каналы router. */
static void md_peer_key(uint8_t key[40], uint64_t group, const uint8_t ed[32]) {
    memcpy(key, &group, sizeof(group)); memcpy(key + sizeof(group), ed, 32);
}

static struct media_super_peer* md_super_peer_find(struct media_delivery_ctx* md, const uint8_t ed[32], uint64_t group_id) {
    if (!md->super_peers) return NULL;
    uint8_t key[40]; md_peer_key(key, group_id, ed);
    struct ll_entry* e = queue_find_data_by_index(md->super_peers, key);
    return e ? (struct media_super_peer*)e->data : NULL;
}

static struct media_super_peer* md_super_peer_add(struct media_delivery_ctx* md, const uint8_t ed[32], uint64_t group_id) {
    struct media_super_peer* sp = md_super_peer_find(md, ed, group_id);
    if (sp) return sp;
    if (!md->super_peers) {
        md->super_peers = queue_new(md->inst->ua, 64, offsetof(struct media_super_peer, group_id), 40, "md_super");
        if (!md->super_peers) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: queue_new(super_peers) failed", MD_ID); return NULL; }
    }
    struct ll_entry* qe = queue_entry_new(sizeof(struct media_super_peer));
    if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: queue_entry_new(super_peer) failed", MD_ID); return NULL; }
    sp = (struct media_super_peer*)qe->data;
    memset(sp, 0, sizeof(*sp));
    memcpy(sp->peer_ed, ed, sizeof(sp->peer_ed));
    sp->group_id = group_id;
    sp->timeout_tb = MEDIA_REPL_TIMEOUT_TB;
    sp->md = md;
    queue_data_put_with_index(md->super_peers, qe);
    sp->timeout_timer = uasync_set_timeout(md->inst->ua, MEDIA_REPL_WATCHDOG_TB, sp, md_repl_watchdog, "md_repl");
    if (!sp->timeout_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: replication watchdog allocation failed", MD_ID);
        queue_remove_data(md->super_peers, qe); queue_entry_free(qe); return NULL;
    }
    return sp;
}

static void md_super_peer_remove(struct media_delivery_ctx* md, const uint8_t ed[32], uint64_t group_id) {
    if (!md->super_peers) return;
    uint8_t key[40]; md_peer_key(key, group_id, ed);
    struct ll_entry* e = queue_find_data_by_index(md->super_peers, key);
    if (!e) return;
    struct media_super_peer* sp = (struct media_super_peer*)e->data;
    md_sql_peer_detach(md, sp); u_free(sp->response_page); sp->response_page = NULL;
    etcp_router_cancel_send_ready(&sp->waiter);
    if (sp->pump) uasync_call_soon_cancel(md->inst->ua, sp->pump);
    if (sp->timeout_timer) { uasync_cancel_timeout(md->inst->ua, sp->timeout_timer); sp->timeout_timer = NULL; }
    if (sp->connection) { conn_mgr_close(sp->connection); sp->connection = NULL; }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: super peer released peer=%s group=%llx", MD_ID,
                sc_public_key_hex(sp->peer_ed, (char[65]){0}), (unsigned long long)sp->group_id);
    queue_remove_data(md->super_peers, e);
    queue_entry_free(e);
}

/* Назначения общие для загрузок и исходящих фоновых подготовок. */
struct media_destination {
    struct media_destination* next;
    struct media_delivery_ctx* owner; /* Destroy отсоединяет; caller освобождает handle даже после restart. */
    char path[MEDIA_PATH_MAX];
};

struct media_destination* media_delivery_reserve_base(struct UTUN_INSTANCE* inst, const char* path, const char* base) {
    char location[MEDIA_PATH_MAX], normalized[MEDIA_PATH_MAX];
    if (!inst || !inst->md.initialized
        || media_index_location(path, base, location) || media_index_path(base, location, normalized)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: destination reservation failed path=%s", MD_ID, path ? path : "(null)");
        return NULL;
    }
    for (struct media_destination* d = inst->md.destinations; d; d = d->next) {
        if (!strcmp(d->path, normalized)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: destination busy path=%s", MD_ID, normalized); return NULL;
        }
    }
    struct media_destination* d = u_calloc(1, sizeof(*d));
    if (!d) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: destination allocation failed", MD_ID); return NULL; }
    memcpy(d->path, normalized, strlen(normalized) + 1);
    d->owner = &inst->md; d->next = inst->md.destinations; inst->md.destinations = d;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: reserved destination=%s", MD_ID, normalized);
    return d;
}
struct media_destination* media_delivery_reserve(struct UTUN_INSTANCE* inst, const char* path) {
    char legacy_base[MEDIA_BASE_MAX]; const char* base = chat_core_media_base(inst);
    if (!base && inst && !db_async_can_write(inst->db_worker) && inst->topo_sqlite_db &&
        !media_index_get_base(inst->topo_sqlite_db, legacy_base)) base = legacy_base;
    if (!inst || !inst->md.initialized || !base) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: destination base unavailable path=%s", MD_ID, path ? path : "(null)"); return NULL;
    }
    return media_delivery_reserve_base(inst, path, base);
}

void media_delivery_release(struct UTUN_INSTANCE* inst, struct media_destination* destination) {
    if (!destination) return;
    if (!destination->owner) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: released detached destination=%s", MD_ID, destination->path);
        u_free(destination); return;
    }
    if (!inst || destination->owner != &inst->md) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: destination release from foreign owner", MD_ID); return;
    }
    struct media_destination** slot = &inst->md.destinations;
    while (*slot && *slot != destination) slot = &(*slot)->next;
    if (!*slot) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: unknown destination reservation", MD_ID); return; }
    *slot = destination->next;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: released destination=%s", MD_ID, destination->path);
    u_free(destination);
}

static void md_block_detach(struct media_block_source* source) {
    if (!source->indexed) return;
    queue_remove_data(source->md->block_sources, source->entry);
    source->indexed = 0;
}

#define MD_BLOCK_KEY_SIZE (sizeof(uint64_t) + 32)
static struct media_block_source* md_block_find(struct media_delivery_ctx* md, uint64_t group,
    const uint8_t media_id[16], const uint8_t block_id[16]) {
    uint8_t key[MD_BLOCK_KEY_SIZE];
    memcpy(key, &group, sizeof(group)); memcpy(key + sizeof(group), media_id, 16); memcpy(key + sizeof(group) + 16, block_id, 16);
    struct ll_entry* e = md->block_sources ? queue_find_data_by_index(md->block_sources, key) : NULL;
    return e ? (struct media_block_source*)e->data : NULL;
}

void md_block_release(struct media_block_source* source) {
    if (!source || --source->refs) return;
    md_block_detach(source);
    if (source->owned) file_io_temp_discard(&source->storage);
    queue_entry_free(source->entry);
}

struct media_block_source* md_block_create(struct media_delivery_ctx* md, uint64_t group_id,
    const uint8_t media_id[16], const uint8_t block_id[16], uint32_t chunk, uint32_t size, const char* destination) {
    if (!md->block_sources)
        md->block_sources = queue_new(md->inst->ua, 64, offsetof(struct media_block_source, group_id), MD_BLOCK_KEY_SIZE, "md_sources");
    if (!md->block_sources) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: source index allocation failed", MD_ID); return NULL; }
    struct ll_entry* entry = queue_entry_new(sizeof(struct media_block_source));
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: block source allocation failed", MD_ID); return NULL; }
    struct media_block_source* source = (struct media_block_source*)entry->data;
    memset(source, 0, sizeof(*source));
    source->entry = entry; source->md = md; source->refs = 1; source->owned = 1;
    source->group_id = group_id; source->chunk = chunk; source->size = size;
    memcpy(source->media_id, media_id, 16); memcpy(source->block_id, block_id, 16);
    if (file_io_temp_open(&source->storage, destination, 0) || file_io_temp_finish(&source->storage)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: block temp creation failed chunk=%u", MD_ID, chunk);
        md_block_release(source); return NULL;
    }
    struct media_block_source* previous = md_block_find(md, group_id, media_id, block_id);
    if (previous) md_block_detach(previous);
    source->indexed = 1; queue_data_put_with_index(md->block_sources, entry);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: block source created chunk=%u size=%u path=%s",
                MD_ID, chunk, size, source->storage.temporary);
    return source;
}

void md_block_publish(struct media_block_source* source, uint32_t available, int verified) {
    source->available = available; source->verified = verified != 0;
    md_block_wake(source, NULL);
}

void md_block_abort(struct media_block_source* source) {
    if (!source || source->failed) return;
    source->failed = 1; md_block_detach(source);
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: block source aborted chunk=%u available=%u/%u",
               MD_ID, source->chunk, source->available, source->size);
    md_block_wake(source, NULL);
}

/* ── file_load helpers (per-file source download tracking) ── */

struct md_file_load* md_file_load_find(struct media_delivery_ctx* md, const uint8_t* media_id) {
    if (!md->file_loads) return NULL;
    struct ll_entry* e = queue_find_data_by_index(md->file_loads, media_id);
    return e ? (struct md_file_load*)e->data : NULL;
}

int md_file_load_inc(struct media_delivery_ctx* md, const uint8_t* media_id, const uint8_t peer[32]) {
    if (!md->file_loads) {
        md->file_loads = queue_new(md->inst->ua, 64, offsetof(struct md_file_load, media_id), 16, "md_fld");
        if (!md->file_loads) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: queue_new(file_loads) failed", MD_ID); return -1; }
    }
    struct md_file_load* fl = md_file_load_find(md, media_id);
    if (!fl) {
        struct ll_entry* qe = queue_entry_new(sizeof(struct md_file_load));
        if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: queue_entry_new(file_load) failed", MD_ID); return -1; }
        fl = (struct md_file_load*)qe->data;
        memset(fl, 0, sizeof(*fl));
        memcpy(fl->media_id, media_id, 16);
        memcpy(qe->data, media_id, 16);
        queue_data_put_with_index(md->file_loads, qe);
    }
    if (fl->active_downloads >= 10) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: file admission counter full count=%d", MD_ID, fl->active_downloads); return -1;
    }
    memcpy(fl->downloader_keys[fl->active_downloads], peer, 32);
    fl->active_downloads++;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: file_load inc media=%02x%02x... peer=%s count=%d",
                MD_ID, media_id[0], media_id[1], sc_public_key_hex(peer, (char[65]){0}), fl->active_downloads);
    return fl->active_downloads;
}

void md_file_load_dec(struct media_delivery_ctx* md, const uint8_t* media_id, const uint8_t peer[32]) {
    struct md_file_load* fl = md_file_load_find(md, media_id);
    if (!fl) return;
    int found = 0;
    for (int i = 0; i < fl->active_downloads; i++) {
        if (!memcmp(fl->downloader_keys[i], peer, 32)) {
            fl->active_downloads--; found = 1;
            memmove(fl->downloader_keys[i], fl->downloader_keys[i + 1], (size_t)(fl->active_downloads - i) * 32);
            memset(fl->downloader_keys[fl->active_downloads], 0, 32);
            break;
        }
    }
    if (!found) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: missing file admission peer=%s count=%d",
                            MD_ID, sc_public_key_hex(peer, (char[65]){0}), fl->active_downloads);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: file_load dec media=%02x%02x... peer=%s count=%d",
                MD_ID, media_id[0], media_id[1], sc_public_key_hex(peer, (char[65]){0}), fl->active_downloads);
    /* remove entry when count reaches 0 */
    if (fl->active_downloads == 0 && md->file_loads) {
        struct ll_entry* e = queue_find_data_by_index(md->file_loads, media_id);
        if (e) { queue_remove_data(md->file_loads, e); queue_entry_free(e); }
    }
}

/* Wire-числа читаются побайтно: packed-поля могут быть невыровнены, endian одинаков на всех платформах. */
static uint64_t md_read_le(const void* ptr, unsigned bytes) {
    const uint8_t* p = ptr;
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static void md_write_le(void* ptr, uint64_t value, unsigned bytes) {
    uint8_t* p = ptr;
    for (unsigned i = 0; i < bytes; i++) p[i] = (uint8_t)(value >> (i * 8));
}

static void md_repl_schedule(struct media_super_peer* peer) {
    if (!peer->pump) peer->pump = uasync_call_soon(peer->md->inst->ua, peer, md_repl_pump);
    if (!peer->pump) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: replication pump allocation failed", MD_ID);
}

/* Курсор читается заново для каждой новой страницы; ошибки БД не превращаются в «курсор 0». */
static int md_repl_request(struct media_super_peer* peer) {
    if (db_async_can_write(peer->md->inst->db_worker)) return md_repl_request_submit(peer);
    struct media_delivery_ctx* md = peer->md;
    struct media_pkt_sync_req req = {.subcmd = MEDIA_SUBCMD_SYNC_REQ};
    if (md_repl_cursor_read(md->db, topo_groups_channel(md->inst->topo_groups, peer->group_id), peer->peer_ed, &req) || media_index_generate_uuid(req.request_id)) return -1;
    peer->request = req;
    peer->request_pending = peer->request_send = 1; peer->dirty = 0;
    peer->deadline_tb = get_time_tb() + peer->timeout_tb;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: SYNC request group=%llx peer=%s uuid=%02x%02x epoch=%02x%02x after=%llu",
                MD_ID, (unsigned long long)peer->group_id, sc_public_key_hex(peer->peer_ed, (char[65]){0}),
                req.request_id[0], req.request_id[1], req.source_epoch[0], req.source_epoch[1],
                (unsigned long long)md_read_le(&req.after_id, 8));
    return 0;
}

/* Один read snapshot: high-water не уменьшается после DELETE, страницы изолированы по router-группе. */
static int md_repl_page_read(sqlite3* db, const char* channel, const uint8_t remote[32], const uint8_t epoch[16],
    const struct media_pkt_sync_req* req, uint8_t packet[MEDIA_SYNC_PACKET_MAX], size_t* size) {
    *size = sizeof(struct media_pkt_sync_resp);
    struct media_pkt_sync_resp* r = (struct media_pkt_sync_resp*)packet;
    memset(packet, 0, MEDIA_SYNC_PACKET_MAX);
    r->subcmd = MEDIA_SUBCMD_SYNC_RESP; memcpy(r->request_id, req->request_id, 16);
    memcpy(r->source_epoch, epoch, 16);
    r->status = MEDIA_SYNC_DB_ERROR;
    sqlite3_stmt* st = NULL;
    uint64_t high = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT seq FROM sqlite_sequence WHERE name='block_availability'", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            if (sqlite3_column_int64(st, 0) < 0) rc = SQLITE_CORRUPT;
            else { high = (uint64_t)sqlite3_column_int64(st, 0); rc = sqlite3_step(st); }
        }
    }
    rc = md_sql_result(db, st, rc, "snapshot high-water"); if (rc) goto failed;
    uint64_t from = memcmp(req->source_epoch, epoch, 16) ? 0 : md_read_le(&req->after_id, 8);
    if (from > high) {
        r->status = MEDIA_SYNC_CURSOR_INVALID;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: source cursor ahead group=%s peer=%s from=%llu high=%llu", MD_ID,
                   channel, sc_public_key_hex(remote, (char[65]){0}),
                   (unsigned long long)from, (unsigned long long)high);
        return SQLITE_OK;
    }
    st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT id,block_uuid,holder,chunk,timestamp,status,version,signature FROM block_availability"
        " WHERE group_id=? AND id>? AND id<=? ORDER BY id LIMIT ?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)from);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)high);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 4, MEDIA_SYNC_MAX_ENTRIES + 1);
    struct media_pkt_sync_entry* rows = (struct media_pkt_sync_entry*)(packet + sizeof(*r));
    unsigned count = 0;
    uint64_t last = from;
    int more = 0;
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == MEDIA_SYNC_MAX_ENTRIES) { more = 1; rc = SQLITE_DONE; break; }
        sqlite3_int64 id = sqlite3_column_int64(st, 0), chunk = sqlite3_column_int64(st, 3);
        sqlite3_int64 ts = sqlite3_column_int64(st, 4);
        int status = sqlite3_column_int(st, 5);
        if (id <= (sqlite3_int64)last || (uint64_t)id > high || sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 16
            || sqlite3_column_type(st, 2) != SQLITE_BLOB || sqlite3_column_bytes(st, 2) != 32
            || chunk < 0 || chunk > UINT32_MAX || ts < 0 || status < 0 || status > 1
            || sqlite3_column_int64(st, 6) <= 0 || sqlite3_column_type(st, 7) != SQLITE_BLOB || sqlite3_column_bytes(st, 7) != 64) {
            rc = SQLITE_CORRUPT; break;
        }
        struct media_pkt_sync_entry* row = &rows[count++];
        md_write_le(&row->id, id, 8); memcpy(row->block_id, sqlite3_column_blob(st, 1), 16);
        memcpy(row->holder, sqlite3_column_blob(st, 2), sizeof(row->holder));
        md_write_le(&row->chunk, chunk, 4); md_write_le(&row->timestamp, ts, 8); row->status = (uint8_t)status;
        md_write_le(&row->version, (uint64_t)sqlite3_column_int64(st, 6), 8); memcpy(row->signature, sqlite3_column_blob(st, 7), 64);
        last = (uint64_t)id;
    }
    rc = md_sql_result(db, st, rc, "snapshot rows"); if (rc) goto failed;
    r->status = MEDIA_SYNC_OK; r->more = (uint8_t)more;
    md_write_le(&r->from_id, from, 8); md_write_le(&r->next_id, more ? last : high, 8);
    md_write_le(&r->num_entries, count, 2);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: SYNC page group=%s peer=%s uuid=%02x%02x epoch=%02x%02x from=%llu next=%llu n=%u more=%d",
                MD_ID, channel, sc_public_key_hex(remote, (char[65]){0}),
                r->request_id[0], r->request_id[1], r->source_epoch[0], r->source_epoch[1],
                (unsigned long long)from, (unsigned long long)(more ? last : high), count, more);
    *size = sizeof(*r) + count * sizeof(*rows); return SQLITE_OK;
failed:
    return rc;
}


static void md_repl_page_error(uint8_t packet[MEDIA_SYNC_PACKET_MAX]) {
    struct media_pkt_sync_resp* response = (void*)packet;
    response->status = MEDIA_SYNC_DB_ERROR; response->more = 0;
    md_write_le(&response->num_entries, 0, 2); md_write_le(&response->from_id, 0, 8); md_write_le(&response->next_id, 0, 8);
}
/* SQL snapshot уже закрыт перед runtime cache/signature checks и отправкой. */
static int md_repl_page_validate(struct media_delivery_ctx* md, uint64_t group, const uint8_t* packet) {
    const struct media_pkt_sync_resp* response = (const void*)packet;
    unsigned count = (unsigned)md_read_le(&response->num_entries, 2);
    const struct media_pkt_sync_entry* rows = (const void*)(packet + sizeof(*response));
    for (unsigned i = 0; i < count; i++) if (md_holder_verify(md, group, rows + i)) return -1;
    return 0;
}
static size_t md_repl_page(struct media_super_peer* peer, uint8_t packet[MEDIA_SYNC_PACKET_MAX]) {
    memset(packet, 0, MEDIA_SYNC_PACKET_MAX);
    struct media_pkt_sync_resp* response = (void*)packet; response->subcmd = MEDIA_SUBCMD_SYNC_RESP;
    memcpy(response->request_id, peer->response.request_id, 16); memcpy(response->source_epoch, peer->md->source_epoch, 16);
    md_repl_page_error(packet);
    if (md_sql_begin(peer->md->db, "BEGIN")) return sizeof(*response);
    size_t size = sizeof(*response);
    int rc = md_repl_page_read(peer->md->db, topo_groups_channel(peer->md->inst->topo_groups, peer->group_id), peer->peer_ed, peer->md->source_epoch, &peer->response, packet, &size);
    if (!rc && md_repl_page_validate(peer->md, peer->group_id, packet)) rc = SQLITE_CORRUPT;
    if (!rc && !md_sql_exec(peer->md->db, "COMMIT")) return size;
    md_sql_rollback(peer->md->db); md_repl_page_error(packet); return sizeof(*response);
}

enum md_repl_read_kind { MD_REPL_CURSOR, MD_REPL_PAGE };
struct md_repl_sql {
    char channel[65];
    enum md_repl_read_kind kind;
    uint64_t group;
    uint8_t remote[32], epoch[16];
    struct media_pkt_sync_req request;
    uint8_t packet[MEDIA_SYNC_PACKET_MAX];
    size_t size;
};
static int md_repl_cursor_read(sqlite3* db, const char* channel, const uint8_t remote[32], struct media_pkt_sync_req* request) {
    sqlite3_stmt* st = NULL; memset(request->source_epoch, 0, 16); md_write_le(&request->after_id, 0, 8);
    int rc = sqlite3_prepare_v2(db, "SELECT source_epoch,last_recv_id FROM super_sync WHERE group_id=? AND peer=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, remote, 32, SQLITE_STATIC);
    if (!rc) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 16 ||
                sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int64(st, 1) < 0) rc = SQLITE_CORRUPT;
            else {
                memcpy(request->source_epoch, sqlite3_column_blob(st, 0), 16);
                md_write_le(&request->after_id, (uint64_t)sqlite3_column_int64(st, 1), 8); rc = sqlite3_step(st);
            }
        }
    }
    return md_sql_result(db, st, rc, "read RX cursor");
}
static int md_repl_read(sqlite3* db, void* arg) {
    struct md_repl_sql* data = arg;
    return data->kind == MD_REPL_CURSOR ? md_repl_cursor_read(db, data->channel, data->remote, &data->request) :
        md_repl_page_read(db, data->channel, data->remote, data->epoch, &data->request, data->packet, &data->size);
}
static int md_repl_read_guard(struct md_sql_job* job) {
    struct md_repl_sql* data = (void*)(job + 1); struct media_super_peer* peer = job->peer;
    return peer && md_group_role(&job->inst->md, data->group, job->inst->my_ed25519_pubkey) == 1 &&
        md_group_role(&job->inst->md, data->group, data->remote) == 1 &&
        (data->kind == MD_REPL_CURSOR ? peer->connected :
            peer->response_pending && !memcmp(&peer->response, &data->request, sizeof(peer->response)));
}
static void md_repl_read_done(struct md_sql_job* job, const struct db_async_result* result) {
    struct md_repl_sql* data = (void*)(job + 1); struct media_super_peer* peer = job->peer;
    if (!peer) return;
    if (!md_repl_read_guard(job)) {
        if (data->kind == MD_REPL_PAGE && peer->response_pending) md_repl_schedule(peer);
        return;
    }
    if (data->kind == MD_REPL_CURSOR) {
        if (result->status) { peer->deadline_tb = get_time_tb() + peer->timeout_tb; return; }
        peer->request = data->request; peer->request_pending = peer->request_send = 1; peer->dirty = 0;
        peer->deadline_tb = get_time_tb() + peer->timeout_tb;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: cursor snapshot ready group=%llx peer=%s after=%llu", MD_ID,
            (unsigned long long)data->group, sc_public_key_hex(data->remote, (char[65]){0}), (unsigned long long)md_read_le(&data->request.after_id, 8));
        md_repl_schedule(peer); return;
    }
    if (result->status || md_repl_page_validate(&job->inst->md, data->group, data->packet)) {
        memset(data->packet, 0, sizeof(data->packet));
        struct media_pkt_sync_resp* response = (void*)data->packet; response->subcmd = MEDIA_SUBCMD_SYNC_RESP;
        memcpy(response->request_id, data->request.request_id, 16); memcpy(response->source_epoch, data->epoch, 16);
        md_repl_page_error(data->packet); data->size = sizeof(*response);
    }
    uint8_t* packet = u_malloc(data->size);
    if (!packet) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: page delivery allocation failed bytes=%zu", MD_ID, data->size); return; }
    memcpy(packet, data->packet, data->size); u_free(peer->response_page); peer->response_page = packet;
    peer->response_size = data->size; md_repl_schedule(peer);
}
static int md_repl_submit_read(struct media_super_peer* peer, enum md_repl_read_kind kind) {
    if (kind == MD_REPL_CURSOR ? peer->request_sql != NULL : peer->page_sql != NULL) return 0;
    struct md_sql_job* job = md_sql_new(peer->md, peer->group_id, sizeof(struct md_repl_sql),
        kind == MD_REPL_CURSOR ? "media cursor" : "media page", DB_ASYNC_READ, md_repl_read, md_repl_read_done);
    if (!job) return -1;
    struct md_repl_sql* data = (void*)(job + 1); data->kind = kind; data->group = peer->group_id; strcpy(data->channel, topo_groups_channel(job->inst->topo_groups, data->group)); memcpy(data->remote, peer->peer_ed, sizeof(data->remote));
    memcpy(data->epoch, peer->md->source_epoch, 16);
    if (kind == MD_REPL_CURSOR) {
        data->request.subcmd = MEDIA_SUBCMD_SYNC_REQ;
        if (media_index_generate_uuid(data->request.request_id)) { md_sql_free(job); return -1; }
        peer->request_sql = job;
    } else { data->request = peer->response; peer->page_sql = job; }
    job->peer = peer; job->peer_required = 1; job->guard = md_repl_read_guard; md_sql_dispatch(job); return 0;
}
static int md_repl_request_submit(struct media_super_peer* peer) { return md_repl_submit_read(peer, MD_REPL_CURSOR); }
static int md_repl_page_submit(struct media_super_peer* peer) { return md_repl_submit_read(peer, MD_REPL_PAGE); }

struct md_merge_sql {
    char channel[65];
    uint64_t group;
    uint8_t remote[32], self_ed[32];
    struct media_pkt_sync_resp response;
    struct media_pkt_sync_entry rows[MEDIA_SYNC_MAX_ENTRIES];
    unsigned count;
    int reset, changed;
};
static int md_repl_merge_write(sqlite3* db, void* arg) {
    struct md_merge_sql* data = arg; sqlite3_stmt* st = NULL; data->changed = 0;
    int rc;
    if (data->reset) {
        rc = sqlite3_prepare_v2(db, "DELETE FROM super_sync WHERE group_id=? AND peer=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, data->channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, data->remote, sizeof(data->remote), SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        return md_sql_result(db, st, rc, "reset ahead cursor");
    }
    for (unsigned i = 0; i < data->count; i++) {
        if (!memcmp(data->rows[i].holder, data->self_ed, sizeof(data->self_ed))) continue;
        int applied = md_ba_merge(db, data->channel, data->rows + i);
        if (applied < 0) return -applied;
        data->changed += applied;
    }
    rc = sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO super_sync(group_id,peer,source_epoch,last_recv_id) VALUES(?,?,?,?)",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, data->channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->remote, sizeof(data->remote), SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, data->response.source_epoch, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)md_read_le(&data->response.next_id, 8));
    if (!rc) rc = sqlite3_step(st);
    return md_sql_result(db, st, rc, "save RX cursor");
}
static int md_repl_merge_guard(struct md_sql_job* job) {
    struct md_merge_sql* data = (void*)(job + 1); struct media_super_peer* peer = job->peer;
    if (!peer || !peer->request_pending || memcmp(peer->request.request_id, data->response.request_id, 16) ||
        md_group_role(&job->inst->md, data->group, job->inst->my_ed25519_pubkey) != 1 ||
        md_group_role(&job->inst->md, data->group, data->remote) != 1) return 0;
    for (unsigned i = 0; i < data->count; i++) if (md_holder_verify(&job->inst->md, data->group, data->rows + i)) return 0;
    return 1;
}
static void md_repl_merge_done(struct md_sql_job* job, const struct db_async_result* result) {
    struct md_merge_sql* data = (void*)(job + 1); struct media_super_peer* peer = job->peer;
    if (result->status || !md_repl_merge_guard(job)) return;
    peer->request_pending = peer->request_send = 0;
    peer->timeout_tb = data->reset ? MEDIA_REPL_TIMEOUT_MAX_TB : MEDIA_REPL_TIMEOUT_TB;
    peer->deadline_tb = get_time_tb() + (data->reset ? peer->timeout_tb : MEDIA_REPL_POLL_TB);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: replication WRITE committed group=%llx peer=%s reset=%d count=%u changed=%d next=%llu", MD_ID,
        (unsigned long long)data->group, sc_public_key_hex(data->remote, (char[65]){0}), data->reset, data->count, data->changed,
        (unsigned long long)md_read_le(&data->response.next_id, 8));
    if (data->changed) md_repl_notify(&job->inst->md, data->group);
    if (!data->reset && (data->response.more || peer->dirty)) {
        if (md_repl_request(peer)) peer->deadline_tb = get_time_tb() + peer->timeout_tb;
        else md_repl_schedule(peer);
    }
}
static void md_repl_merge_submit(struct media_super_peer* peer, const struct media_pkt_sync_resp* response,
    const struct media_pkt_sync_entry* rows, unsigned count, int reset) {
    if (peer->merge_sql) return;
    struct md_sql_job* job = md_sql_new(peer->md, peer->group_id, sizeof(struct md_merge_sql), "media merge", DB_ASYNC_WRITE,
        md_repl_merge_write, md_repl_merge_done);
    if (!job) return;
    struct md_merge_sql* data = (void*)(job + 1); data->group = peer->group_id; strcpy(data->channel, topo_groups_channel(job->inst->topo_groups, data->group)); memcpy(data->remote, peer->peer_ed, sizeof(data->remote));
    memcpy(data->self_ed, peer->md->inst->my_ed25519_pubkey, sizeof(data->self_ed));
    data->response = *response; data->count = count; data->reset = reset; if (count) memcpy(data->rows, rows, count * sizeof(*rows));
    peer->merge_sql = job; job->peer = peer; job->peer_required = 1; job->guard = md_repl_merge_guard; md_sql_dispatch(job);
}

/* Watchdog и waiter только планируют pump; это безопасно при синхронном callback очереди. */
static void md_repl_ready(struct ll_queue* queue, void* arg) { (void)queue; md_repl_schedule(arg); }

static void md_repl_pump(void* arg) {
    struct media_super_peer* peer = arg;
    struct media_delivery_ctx* md = peer->md;
    peer->pump = NULL;
    if (!md->initialized || md_group_role(md, peer->group_id, md->inst->my_ed25519_pubkey) != 1
        || md_group_role(md, peer->group_id, peer->peer_ed) != 1) return;
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get_key(md->inst, peer->group_id, peer->peer_ed, ETCP_RT_ID_MEDIA_DELIVERY);
    if (!route) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: replication router channel allocation failed", MD_ID); return;
    }
    etcp_router_cancel_send_ready(&peer->waiter);
    /* Максимум три пакета за вызов: ответ, свой запрос и подсказка. */
    for (int budget = 0; budget < 3; budget++) {
        if (!peer->response_pending && !(peer->connected && (peer->request_send || peer->changed))) return;
        if (!etcp_router_conn_send_q_has_room(route)) {
            etcp_router_conn_on_send_ready(route, &peer->waiter, md_repl_ready, peer);
            return;
        }
        uint8_t packet[MEDIA_SYNC_PACKET_MAX];
        const uint8_t* data = packet;
        size_t len = 1;
        int kind = peer->response_pending ? 0 : peer->request_send ? 1 : 2;
        if (kind == 0 && db_async_can_write(md->inst->db_worker)) {
            if (!peer->response_page) { md_repl_page_submit(peer); return; }
            data = peer->response_page; len = peer->response_size;
        } else if (kind == 0) len = md_repl_page(peer, packet);
        else if (kind == 1) { data = (const uint8_t*)&peer->request; len = sizeof(peer->request); }
        else packet[0] = MEDIA_SUBCMD_CHANGED;
        if (md_send_key(md->inst, peer->group_id, peer->peer_ed, data, len, 0)) return;
        if (kind == 0) { peer->response_pending = 0; u_free(peer->response_page); peer->response_page = NULL; peer->response_size = 0; }
        else if (kind == 1) peer->request_send = 0;
        else peer->changed = 0;
    }
}

static void md_repl_watchdog(void* arg) {
    struct media_super_peer* peer = arg;
    peer->timeout_timer = NULL;
    if (!peer->md->initialized) return;
    uint64_t now = get_time_tb();
    if (peer->connected && !peer->connection && !etcp_router_peer_reachable(peer->md->inst, peer->group_id, peer->peer_ed, ETCP_RT_ID_MEDIA_DELIVERY)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: replication route lost group=%llx peer=%s", MD_ID,
            (unsigned long long)peer->group_id, sc_public_key_hex(peer->peer_ed, (char[65]){0}));
        peer->connected = peer->request_pending = peer->request_send = 0;
    }
    if (peer->connected && now >= peer->deadline_tb) {
        if (peer->request_pending) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: SYNC retry group=%llx peer=%s uuid=%02x%02x delay=%u",
                       MD_ID, (unsigned long long)peer->group_id, sc_public_key_hex(peer->peer_ed, (char[65]){0}),
                       peer->request.request_id[0], peer->request.request_id[1], peer->timeout_tb);
            peer->timeout_tb = peer->timeout_tb < MEDIA_REPL_TIMEOUT_MAX_TB / 2
                ? peer->timeout_tb * 2 : MEDIA_REPL_TIMEOUT_MAX_TB;
            peer->request_send = 1;
        } else if (md_repl_request(peer)) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: SYNC request initialization failed", MD_ID);
        peer->deadline_tb = now + peer->timeout_tb;
    } else if (!peer->connection && now >= peer->deadline_tb) {
        md_super_connect(peer->md, peer->peer_ed, peer->group_id);
        peer->deadline_tb = now + peer->timeout_tb;
    }
    if (peer->response_pending || (peer->connected && (peer->request_send || peer->changed))) md_repl_schedule(peer);
    peer->timeout_timer = uasync_set_timeout(peer->md->inst->ua, MEDIA_REPL_WATCHDOG_TB, peer, md_repl_watchdog, "md_repl");
    if (!peer->timeout_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: replication watchdog lost; releasing peer", MD_ID);
        md_super_peer_remove(peer->md, peer->peer_ed, peer->group_id);
    }
}

/* Только после commit: потеря CHANGED безопасна благодаря периодической сверке. */
static void md_repl_notify(struct media_delivery_ctx* md, uint64_t group_id) {
    for (struct ll_entry* e = md->super_peers ? md->super_peers->head : NULL; e; e = e->next) {
        struct media_super_peer* peer = (struct media_super_peer*)e->data;
        if (peer->group_id != group_id) continue;
        peer->changed = 1; md_repl_schedule(peer);
    }
}

/* ── send via etcp_router helper ── */

/* Репликация использует force=0; остальные команды сохраняют существующую политику очереди. */
int md_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t* data, size_t len, int force) {
    if (!inst || !peer || !data || !len || len >= UINT16_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: invalid send bytes=%zu", MD_ID, len); return -1;
    }
    if (!topo_group_chat_key_allowed(inst, group, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: send denied group=%llx peer=%s command=%u", MD_ID,
            (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), data[0]); return -1;
    }
    struct ll_entry* entry = queue_entry_new(0);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: send entry allocation failed", MD_ID); return -1; }
    entry->dgram = u_malloc(len + 1);
    if (!entry->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: send payload allocation failed", MD_ID); queue_entry_free(entry); return -1; }
    entry->dgram[0] = ETCP_RT_ID_MEDIA_DELIVERY;
    memcpy(entry->dgram + 1, data, len); entry->len = (uint16_t)(len + 1);
    int rc = etcp_route_send_key(inst, group, peer, entry, force, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: send failed rc=%d group=%llx peer=%s command=%u", MD_ID,
        rc, (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), data[0]);
    return rc;
}

/* ── subcommand handlers ── */

static void md_handle_serve_reg(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t ed[32],
                                 const uint8_t* data, size_t len) {
    const struct media_pkt_serve_reg* pkt = (const void*)data;
    const uint8_t* group_ed = md_group_key(md->inst, group_id);
    if (len != sizeof(*pkt) || !group_ed || memcmp(pkt->group_ed, group_ed, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: SERVE_REG invalid size/group bytes=%zu group=%llx", MD_ID, len,
                   (unsigned long long)group_id); return;
    }

    if (!md->served_nodes) {
        md->served_nodes = queue_new(md->inst->ua, 64, offsetof(struct media_served_node, group_id), 40, "md_served");
        if (!md->served_nodes) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: queue_new(served_nodes) failed", MD_ID); return; }
    }
    uint8_t key[40]; md_peer_key(key, group_id, ed);
    struct ll_entry* e = queue_find_data_by_index(md->served_nodes, key);
    if (!e) {
        struct ll_entry* qe = queue_entry_new(sizeof(struct media_served_node));
        if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: queue_entry_new(served_node) failed", MD_ID); return; }
        struct media_served_node* sn = (struct media_served_node*)qe->data;
        memset(sn, 0, sizeof(*sn));
        sn->group_id = group_id; memcpy(sn->peer_ed, ed, sizeof(sn->peer_ed));
        sn->joined_at = (int64_t)time(NULL);
        queue_data_put_with_index(md->served_nodes, qe);
    }

    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: SERVE_REG peer=%s group=%016llx",
               MD_ID, sc_public_key_hex(ed, (char[65]){0}), (unsigned long long)group_id);
}

static void md_handle_query(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t peer[32],
                             const uint8_t* data, size_t len) {
    if (len < MEDIA_QUERY_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: short QUERY bytes=%zu", MD_ID, len); return;
    }
    const struct media_pkt_query* q = (const struct media_pkt_query*)data;
    if (!q->num_blocks || q->num_blocks > MEDIA_QUERY_MAX_BLOCKS
        || len != MEDIA_QUERY_HDR_SIZE + (size_t)q->num_blocks * 16) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: invalid QUERY blocks=%u bytes=%zu", MD_ID, q->num_blocks, len); return;
    }
    if (!topo_group_chat_key_allowed(md->inst, group_id, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: QUERY denied group=%llx peer=%s", MD_ID,
            (unsigned long long)group_id, sc_public_key_hex(peer, (char[65]){0})); return;
    }
    if (db_async_can_write(md->inst->db_worker)) { md_query_submit(md, group_id, peer, q); return; }
    uint8_t response[MEDIA_QUERY_RESP_HDR_SIZE + MEDIA_QUERY_MAX_BLOCKS * sizeof(struct media_pkt_query_resp_entry)] = {0};
    struct media_pkt_query_resp* r = (struct media_pkt_query_resp*)response;
    r->subcmd = MEDIA_SUBCMD_QUERY_RESP;
    memcpy(r->media_id, q->media_id, 16); memcpy(r->request_id, q->request_id, 16);
    r->num_blocks = q->num_blocks;
    struct media_pkt_query_resp_entry* results = (struct media_pkt_query_resp_entry*)(response + MEDIA_QUERY_RESP_HDR_SIZE);
    for (unsigned i = 0; i < q->num_blocks; i++) {
        memcpy(results[i].block_id, data + MEDIA_QUERY_HDR_SIZE + i * 16, 16);
        if (!md->db || md_query_holders_read(md->db, md->inst->my_ed25519_pubkey, topo_groups_channel(md->inst->topo_groups, group_id), peer, q->media_id, &results[i])) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: QUERY batch failed group=%016llx block=%u db=%p",
                        MD_ID, (unsigned long long)group_id, i, (void*)md->db);
            r->status = MEDIA_QUERY_DB_ERROR; r->num_blocks = 0; break;
        }
    }
    int rc = md_send_key(md->inst, group_id, peer, response,
                     MEDIA_QUERY_RESP_HDR_SIZE + (size_t)r->num_blocks * sizeof(*results), 1);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: QUERY peer=%s group=%016llx blocks=%u status=%u send=%d",
                MD_ID, sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)group_id, q->num_blocks, r->status, rc);
}

/* Локальное HAVE/PROCESSING проходит тот же merge, что и страница, с самостоятельным commit. */
static void md_handle_announcement(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t peer_ed[32],
                                    const uint8_t* data, size_t len, int status) {
    const char* channel = topo_groups_channel(md->inst->topo_groups, group_id);
    if (len != MEDIA_HAVE_BLOCK_SIZE || !topo_group_chat_key_allowed(md->inst, group_id, peer_ed) ||
        group_policy_member_role_key(md->inst, channel, md->inst->my_ed25519_pubkey, GROUP_MEMBER_SUPERNODE) != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: rejected announcement group=%llx peer=%s bytes=%zu", MD_ID,
                   (unsigned long long)group_id, sc_public_key_hex(peer_ed, (char[65]){0}), len); return;
    }
    const struct media_pkt_have_block* hb = (const struct media_pkt_have_block*)data;
    const uint8_t* group_ed = md_group_key(md->inst, group_id);
    if (!group_ed || memcmp(hb->group_ed, group_ed, 32) || hb->timestamp < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: announcement group/timestamp mismatch", MD_ID); return;
    }
    struct media_pkt_sync_entry row = {0};
    memcpy(row.block_id, hb->block_id, 16); memcpy(row.holder, peer_ed, sizeof(row.holder));
    md_write_le(&row.chunk, hb->chunk, 4); md_write_le(&row.timestamp, (uint64_t)hb->timestamp, 8);
    memcpy(&row.version, &hb->version, 8); row.status = status; memcpy(row.signature, hb->signature, 64);
    if (md_holder_verify(md, group_id, &row)) return;
    if (db_async_can_write(md->inst->db_worker)) { md_announce_submit(md, group_id, peer_ed, hb, &row, status); return; }
    int changed = -1;
    if (!md_sql_begin(md->db, "BEGIN IMMEDIATE")) {
        changed = md_ba_merge(md->db, topo_groups_channel(md->inst->topo_groups, group_id), &row);
        if (changed < 0 || md_sql_exec(md->db, "COMMIT")) {
            md_sql_rollback(md->db); changed = -1;
        }
    }
    if (status) {
        struct media_pkt_have_block_ack ack = {.subcmd = MEDIA_SUBCMD_HAVE_BLOCK_ACK,
            .status = changed < 0 ? 2 : changed ? 0 : 1};
        memcpy(ack.media_id, hb->media_id, 16); memcpy(ack.block_id, hb->block_id, 16);
        md_send_key(md->inst, group_id, peer_ed, (const uint8_t*)&ack, sizeof(ack), 1);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: announcement group=%llx holder=%s status=%d changed=%d",
                MD_ID, (unsigned long long)group_id, sc_public_key_hex(peer_ed, (char[65]){0}), status, changed);
    if (changed > 0) md_repl_notify(md, group_id);
}

/* До выделения контекста проверяем точный формат и подписанные роли именно router-группы. */
static void md_handle_sync_req(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t from_ed[32],
                               const uint8_t* data, size_t len) {
    if (len != MEDIA_SYNC_REQ_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: malformed SYNC_REQ bytes=%zu", MD_ID, len); return;
    }
    const struct media_pkt_sync_req* q = (const struct media_pkt_sync_req*)data;
    if (md_read_le(&q->after_id, 8) > INT64_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: invalid SYNC_REQ cursor", MD_ID); return;
    }
    struct media_super_peer* peer = md_super_peer_add(md, from_ed, group_id);
    if (!peer) return;
    u_free(peer->response_page); peer->response_page = NULL; peer->response_size = 0;
    peer->response = *q; peer->response_pending = 1;
    md_super_connect(md, from_ed, group_id);
    md_repl_schedule(peer);
}

/* Ошибка/дубликат не меняет ожидающий запрос. Только полный commit открывает следующую страницу. */
static void md_handle_sync_resp(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t from_ed[32],
                                const uint8_t* data, size_t len) {
    if (len < MEDIA_SYNC_RESP_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: short SYNC_RESP bytes=%zu", MD_ID, len); return;
    }
    const struct media_pkt_sync_resp* r = (const struct media_pkt_sync_resp*)data;
    struct media_super_peer* peer = md_super_peer_find(md, from_ed, group_id);
    if (!peer || !peer->request_pending || memcmp(r->request_id, peer->request.request_id, 16)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: stale SYNC_RESP group=%llx peer=%s uuid=%02x%02x", MD_ID,
                    (unsigned long long)group_id, sc_public_key_hex(from_ed, (char[65]){0}), r->request_id[0], r->request_id[1]); return;
    }
    unsigned count = (unsigned)md_read_le(&r->num_entries, 2);
    uint64_t from = md_read_le(&r->from_id, 8), next = md_read_le(&r->next_id, 8);
    static const uint8_t zero[16] = {0};
    if (count > MEDIA_SYNC_MAX_ENTRIES || len != sizeof(*r) + count * sizeof(struct media_pkt_sync_entry)
        || r->more > 1 || r->status > MEDIA_SYNC_CURSOR_INVALID || !memcmp(r->source_epoch, zero, 16)) goto malformed;
    if (r->status != MEDIA_SYNC_OK) {
        if (count || r->more || from || next) goto malformed;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: SYNC error group=%llx peer=%s status=%u uuid=%02x%02x",
                   MD_ID, (unsigned long long)group_id, sc_public_key_hex(from_ed, (char[65]){0}),
                   r->status, r->request_id[0], r->request_id[1]);
        if (r->status == MEDIA_SYNC_CURSOR_INVALID) {
            if (memcmp(r->source_epoch, peer->request.source_epoch, 16)) goto malformed;
            if (db_async_can_write(md->inst->db_worker)) { md_repl_merge_submit(peer, r, NULL, 0, 1); return; }
            if (md_sql_begin(md->db, "BEGIN IMMEDIATE")) return;
            sqlite3_stmt* st = NULL;
            int rc = sqlite3_prepare_v2(md->db, "DELETE FROM super_sync WHERE group_id=? AND peer=?", -1, &st, NULL);
            if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, topo_groups_channel(md->inst->topo_groups, group_id), -1, SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, from_ed, 32, SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_step(st);
            if (md_sql_finish(md->db, st, rc, "reset ahead cursor") || md_sql_exec(md->db, "COMMIT")) {
                md_sql_rollback(md->db); return;
            }
            peer->request_pending = peer->request_send = 0;
            peer->timeout_tb = MEDIA_REPL_TIMEOUT_MAX_TB;
            peer->deadline_tb = get_time_tb() + peer->timeout_tb;
        }
        return;
    }
    uint64_t expected = memcmp(r->source_epoch, peer->request.source_epoch, 16)
        ? 0 : md_read_le(&peer->request.after_id, 8);
    if (from != expected || next < from || next > INT64_MAX || (r->more && count != MEDIA_SYNC_MAX_ENTRIES)) goto malformed;
    const struct media_pkt_sync_entry* rows = (const struct media_pkt_sync_entry*)(data + sizeof(*r));
    uint64_t last = from;
    for (unsigned i = 0; i < count; i++) {
        uint64_t id = md_read_le(&rows[i].id, 8);
        if (id <= last || id > next || !memcmp(rows[i].holder, (const uint8_t[32]){0}, sizeof(rows[i].holder))
            || md_read_le(&rows[i].timestamp, 8) > INT64_MAX || rows[i].status > 1) goto malformed;
        for (unsigned j = 0; j < i; j++) {
            if (memcmp(rows[i].block_id, rows[j].block_id, 16)) continue;
            if (!memcmp(rows[i].holder, rows[j].holder, sizeof(rows[i].holder))) goto malformed;
        }
        if (md_holder_verify(md, group_id, &rows[i])) return;
        last = id;
    }
    if (r->more && last != next) goto malformed;
    if (db_async_can_write(md->inst->db_worker)) { md_repl_merge_submit(peer, r, rows, count, 0); return; }
    if (md_sql_begin(md->db, "BEGIN IMMEDIATE")) return;
    int changed = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!memcmp(rows[i].holder, md->inst->my_ed25519_pubkey, sizeof(rows[i].holder))) continue;
        int applied = md_ba_merge(md->db, topo_groups_channel(md->inst->topo_groups, group_id), &rows[i]);
        if (applied < 0) { md_sql_rollback(md->db); return; }
        changed += applied;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(md->db, "INSERT OR REPLACE INTO super_sync"
        "(group_id,peer,source_epoch,last_recv_id) VALUES(?,?,?,?)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, topo_groups_channel(md->inst->topo_groups, group_id), -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, from_ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, r->source_epoch, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)next);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (md_sql_finish(md->db, st, rc, "save RX cursor") || md_sql_exec(md->db, "COMMIT")) {
        md_sql_rollback(md->db); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: SYNC committed group=%llx peer=%s uuid=%02x%02x epoch=%02x%02x from=%llu next=%llu n=%u changed=%d more=%u dirty=%u",
                MD_ID, (unsigned long long)group_id, sc_public_key_hex(from_ed, (char[65]){0}), r->request_id[0], r->request_id[1],
                r->source_epoch[0], r->source_epoch[1], (unsigned long long)from, (unsigned long long)next,
                count, changed, r->more, peer->dirty);
    int again = r->more || peer->dirty;
    peer->request_pending = peer->request_send = 0; peer->timeout_tb = MEDIA_REPL_TIMEOUT_TB;
    peer->deadline_tb = get_time_tb() + MEDIA_REPL_POLL_TB;
    if (changed) md_repl_notify(md, group_id);
    if (again) {
        if (md_repl_request(peer)) peer->deadline_tb = get_time_tb() + peer->timeout_tb;
        else md_repl_schedule(peer);
    }
    return;
malformed:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: malformed SYNC_RESP group=%llx peer=%s bytes=%zu count=%u status=%u",
               MD_ID, (unsigned long long)group_id, sc_public_key_hex(from_ed, (char[65]){0}), len, count, r->status);
}

/* CM владеет путём целиком: raw ETCP DOWN не отменяет исправный непрямой путь. */
static void md_super_conn_cb(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t group_id,
                              enum conn_mgr_event event, void* arg) {
    struct media_super_peer* peer = arg; struct media_delivery_ctx* md = peer->md;
    if (!md->initialized) return;
    if (!h || peer->connection != h || peer->group_id != group_id) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: stale replication CM callback", MD_ID); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: replication CM group=%llx peer=%s event=%d", MD_ID,
                (unsigned long long)group_id, sc_public_key_hex(peer->peer_ed, (char[65]){0}), event);
    peer->connected = event == CONN_EVENT_UP;
    peer->request_pending = peer->request_send = 0;
    if (event == CONN_EVENT_UP) {
        peer->timeout_tb = MEDIA_REPL_TIMEOUT_TB;
        if (md_repl_request(peer)) peer->deadline_tb = get_time_tb() + peer->timeout_tb;
        md_repl_schedule(peer);
        return;
    }
    peer->response_pending = 0;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: replication path unavailable group=%llx peer=%s event=%d", MD_ID,
               (unsigned long long)group_id, sc_public_key_hex(peer->peer_ed, (char[65]){0}), event);
    etcp_router_cancel_send_ready(&peer->waiter);
    if (peer->pump) { uasync_call_soon_cancel(md->inst->ua, peer->pump); peer->pump = NULL; }
    if (event == CONN_EVENT_TIMEOUT) {
        if (peer->connection) conn_mgr_close(peer->connection);
        peer->connection = NULL; peer->deadline_tb = get_time_tb() + peer->timeout_tb;
    }
}

/* ── handle incoming BLOCK_REQ (admission control + streaming) ── */

#define MD_MAX_STREAMS 3

struct stream_ctx {
    struct stream_ctx* next;
    struct media_delivery_ctx* md;
    struct media_block_source* source;
    uint64_t group_id;
    uint8_t dst_ed[32]; /* Канал, зарезервированный при принятии запроса. */
    uint8_t transfer_id[16];
    uint32_t offset;
    uint64_t last_progress_tb;
    void* pump;
    void* timer;
    struct queue_waiter_handle waiter;
};

int media_delivery_path_busy(struct UTUN_INSTANCE* inst, const char* path) {
    for (struct media_destination* d = inst->md.destinations; d; d = d->next)
        if (!strcmp(d->path, path)) return 1;
    for (struct stream_ctx* s = inst->md.streams; s; s = s->next)
        if (!strcmp(s->source->storage.temporary, path)) return 1;
    return 0;
}

static void stream_pump(void* arg);

static void stream_free(struct stream_ctx* sc) {
    struct media_delivery_ctx* md = sc->md;
    etcp_router_cancel_send_ready(&sc->waiter);
    if (sc->pump) uasync_call_soon_cancel(md->inst->ua, sc->pump);
    if (sc->timer) uasync_cancel_timeout(md->inst->ua, sc->timer);
    struct stream_ctx** slot = &md->streams;
    while (*slot && *slot != sc) slot = &(*slot)->next;
    if (*slot) *slot = sc->next;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: TX released peer=%s chunk=%u sent=%u/%u attempt=%02x%02x",
                MD_ID, sc_public_key_hex(sc->dst_ed, (char[65]){0}), sc->source->chunk, sc->offset, sc->source->size,
                sc->transfer_id[0], sc->transfer_id[1]);
    if (!sc->source->owned) {
        md_file_load_dec(md, sc->source->media_id, sc->dst_ed);
        media_delivery_stream_done(md->inst);
    }
    md_block_release(sc->source);
    u_free(sc);
}

static int stream_schedule(struct stream_ctx* sc) {
    if (!sc->pump) sc->pump = uasync_call_soon(sc->md->inst->ua, sc, stream_pump);
    if (sc->pump) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot schedule TX peer=%s chunk=%u",
                MD_ID, sc_public_key_hex(sc->dst_ed, (char[65]){0}), sc->source->chunk);
    return -1;
}

static void stream_ready(struct ll_queue* queue, void* arg) {
    (void)queue;
    struct stream_ctx* sc = arg;
    if (stream_schedule(sc)) stream_free(sc);
}

static void md_block_wake(struct media_block_source* source, struct stream_ctx* executing) {
    struct stream_ctx* sc = source->md->streams;
    while (sc) {
        struct stream_ctx* next = sc->next;
        if (sc != executing && sc->source == source && stream_schedule(sc)) stream_free(sc);
        sc = next;
    }
}

static void stream_watchdog(void* arg) {
    struct stream_ctx* sc = arg;
    sc->timer = NULL;
    uint64_t timeout = sc->md->dl_stall_timeout_tb > 0 ? (uint64_t)sc->md->dl_stall_timeout_tb : MEDIA_DL_STALL_TIMEOUT_TB;
    if (get_time_tb() - sc->last_progress_tb >= timeout) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: TX stalled peer=%s chunk=%u offset=%u available=%u verified=%u",
                   MD_ID, sc_public_key_hex(sc->dst_ed, (char[65]){0}), sc->source->chunk, sc->offset,
                   sc->source->available, sc->source->verified);
        stream_free(sc); return;
    }
    sc->timer = uasync_set_timeout(sc->md->inst->ua, 2000, sc, stream_watchdog, "md_tx");
    if (!sc->timer || stream_schedule(sc)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: TX watchdog scheduling failed", MD_ID); stream_free(sc);
    }
}

/* Единственный путь выдачи готового диапазона и растущего temp. Waiter может сработать синхронно:
 * callback только ставит pump, не входит в него рекурсивно. DONE проходит тот же backpressure, что DATA. */
static void stream_pump(void* arg) {
    struct stream_ctx* sc = arg;
    struct media_delivery_ctx* md = sc->md;
    struct media_block_source* source = sc->source;
    sc->pump = NULL;
    if (!md->initialized) { stream_free(sc); return; }
    if (!topo_group_chat_key_allowed(md->inst, sc->group_id, sc->dst_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: TX revoked group=%llx peer=%s", MD_ID,
                   (unsigned long long)sc->group_id, sc_public_key_hex(sc->dst_ed, (char[65]){0}));
        stream_free(sc); return;
    }
    etcp_router_cancel_send_ready(&sc->waiter);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_lookup(md->inst, sc->group_id, sc->dst_ed, ETCP_RT_ID_MEDIA_DELIVERY);
    if (!route) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: TX channel lost group=%llx peer=%s", MD_ID,
                   (unsigned long long)sc->group_id, sc_public_key_hex(sc->dst_ed, (char[65]){0}));
        stream_free(sc); return;
    }
    for (int budget = 0; budget < 4; budget++) {
        if (!source->failed && sc->offset == source->available && !source->verified) return;
        if (!etcp_router_conn_send_q_has_room(route)) {
            etcp_router_conn_on_send_ready(route, &sc->waiter, stream_ready, sc);
            return;
        }
        if (source->failed || (source->verified && sc->offset == source->size)) {
            struct media_pkt_block_done done = {.subcmd = MEDIA_SUBCMD_BLOCK_DONE, .chunk = source->chunk,
                .total_size = source->size, .status = source->failed ? MEDIA_BLOCK_UNAVAILABLE : MEDIA_BLOCK_OK};
            memcpy(done.media_id, source->media_id, 16); memcpy(done.block_id, source->block_id, 16);
            memcpy(done.transfer_id, sc->transfer_id, 16);
            if (!etcp_router_conn_send(route, (const uint8_t*)&done, sizeof(done), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT))
                DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: TX DONE peer=%s chunk=%u bytes=%u status=%u",
                           MD_ID, sc_public_key_hex(sc->dst_ed, (char[65]){0}), source->chunk, sc->offset, done.status);
            else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: TX DONE failed peer=%s chunk=%u",
                             MD_ID, sc_public_key_hex(sc->dst_ed, (char[65]){0}), source->chunk);
            stream_free(sc); return;
        }
        uint32_t count = source->available - sc->offset;
        if (count > MEDIA_BLOCK_DATA_MAX) count = MEDIA_BLOCK_DATA_MAX;
        uint8_t packet[MEDIA_BLOCK_CHUNK_HDR_SIZE + MEDIA_BLOCK_DATA_MAX];
        struct media_pkt_block_chunk* ch = (struct media_pkt_block_chunk*)packet;
        memset(ch, 0, sizeof(*ch)); ch->subcmd = MEDIA_SUBCMD_BLOCK_CHUNK;
        memcpy(ch->media_id, source->media_id, 16); memcpy(ch->block_id, source->block_id, 16);
        memcpy(ch->transfer_id, sc->transfer_id, 16);
        ch->chunk = source->chunk; ch->offset = sc->offset; ch->data_len = (uint16_t)count;
        FILE* file = file_io_open_read(source->storage.temporary);
        int io_error = !file;
        if (file) {
            if (fseeko(file, (off_t)(source->base + sc->offset), SEEK_SET)
                || fread(packet + MEDIA_BLOCK_CHUNK_HDR_SIZE, 1, count, file) != count) io_error = 1;
            if (fclose(file)) io_error = 1;
        }
        if (io_error) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: TX read failed path=%s chunk=%u offset=%u count=%u errno=%d",
                        MD_ID, source->storage.temporary, source->chunk, sc->offset, count, errno);
            source->failed = 1; md_block_detach(source); md_block_wake(source, sc);
            continue;
        }
        if (etcp_router_conn_send(route, packet, sizeof(*ch) + count, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: TX DATA failed peer=%s chunk=%u offset=%u",
                        MD_ID, sc_public_key_hex(sc->dst_ed, (char[65]){0}), source->chunk, sc->offset);
            stream_free(sc); return;
        }
        sc->offset += count; sc->last_progress_tb = get_time_tb();
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: TX peer=%s chunk=%u offset=%u available=%u/%u verified=%u",
                    MD_ID, sc_public_key_hex(sc->dst_ed, (char[65]){0}), source->chunk, sc->offset,
                    source->available, source->size, source->verified);
    }
    if (stream_schedule(sc)) stream_free(sc);
}

static void md_block_reply(struct media_delivery_ctx* md, uint64_t group, const uint8_t peer[32],
                           const struct media_pkt_block_req* request, int overloaded) {
    if (overloaded) {
        struct media_pkt_block_overloaded reply = {.subcmd = MEDIA_SUBCMD_BLOCK_OVERLOADED,
            .chunk = request->chunk, .retry_after_ms = 2000};
        memcpy(reply.media_id, request->media_id, 16); memcpy(reply.block_id, request->block_id, 16);
        memcpy(reply.transfer_id, request->transfer_id, 16);
        md_send_key(md->inst, group, peer, (const uint8_t*)&reply, sizeof(reply), 1);
    } else {
        struct media_pkt_block_done reply = {.subcmd = MEDIA_SUBCMD_BLOCK_DONE, .chunk = request->chunk,
            .status = MEDIA_BLOCK_UNAVAILABLE};
        memcpy(reply.media_id, request->media_id, 16); memcpy(reply.block_id, request->block_id, 16);
        memcpy(reply.transfer_id, request->transfer_id, 16);
        md_send_key(md->inst, group, peer, (const uint8_t*)&reply, sizeof(reply), 1);
    }
}

static void md_stream_start(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t peer[32],
    const struct media_pkt_block_req* req, struct media_block_source* source) {
    for (struct stream_ctx* stream = md->streams; stream; stream = stream->next) {
        if (stream->group_id == group_id && !memcmp(stream->dst_ed, peer, 32) && !memcmp(stream->transfer_id, req->transfer_id, 16)) {
            md_block_release(source); return;
        }
    }
    struct md_file_load* load = md_file_load_find(md, req->media_id);
    if (!source->owned && (md->active_streams >= MD_MAX_STREAMS ||
        (load && md->max_downloads_per_file && load->active_downloads >= md->max_downloads_per_file))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: indexed admission changed while snapshot was pending", MD_ID);
        md_block_release(source); md_block_reply(md, group_id, peer, req, 1); return;
    }
    struct stream_ctx* sc = u_calloc(1, sizeof(*sc));
    struct ETCP_ROUTER_CONN* route = sc ? etcp_router_conn_get_key(md->inst, group_id, peer, ETCP_RT_ID_MEDIA_DELIVERY) : NULL;
    if (!route
        || (!source->owned && md_file_load_inc(md, req->media_id, peer) < 0)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot start TX peer=%s chunk=%u", MD_ID,
                    sc_public_key_hex(peer, (char[65]){0}), req->chunk);
        u_free(sc); md_block_release(source); md_block_reply(md, group_id, peer, req, 0); return;
    }
    sc->md = md; sc->source = source; sc->group_id = group_id;
    memcpy(sc->dst_ed, route->remote_ed, 32);
    memcpy(sc->transfer_id, req->transfer_id, 16); sc->last_progress_tb = get_time_tb();
    sc->next = md->streams; md->streams = sc;
    if (!source->owned) md->active_streams++;
    md->streams_started++;
    sc->timer = uasync_set_timeout(md->inst->ua, 2000, sc, stream_watchdog, "md_tx");
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: TX started peer=%s group=%016llx chunk=%u size=%u available=%u relay=%u",
               MD_ID, sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)group_id, source->chunk,
               source->size, source->available, source->owned);
    if (!sc->timer || stream_schedule(sc)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: TX startup scheduling failed", MD_ID); stream_free(sc);
    }
}


struct md_provider_sql {
    char channel[65];
    uint64_t group;
    uint8_t peer[32], self_ed[32];
    struct media_pkt_block_req request;
    char path[MEDIA_PATH_MAX];
    int64_t size, offset, file_size;
    int found;
};
static int md_provider_read(sqlite3* db, void* arg) {
    struct md_provider_sql* data = arg; data->found = 0; data->path[0] = 0;
    char location[MEDIA_PATH_MAX]; const char* channel = data->channel;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT location,chunk_size,chunk,offset,file_size FROM media_public_files"
        " WHERE block_id=? AND media_id=? AND holder=? AND chat_id=? AND chunk=? LIMIT 1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->request.block_id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->request.media_id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, data->self_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 4, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 5, data->request.chunk);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* value = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0);
        data->size = sqlite3_column_int64(st, 1); data->offset = sqlite3_column_int64(st, 3); data->file_size = sqlite3_column_int64(st, 4);
        int valid = sqlite3_column_type(st, 0) == SQLITE_TEXT && value && bytes > 0 && bytes < MEDIA_PATH_MAX &&
            !memchr(value, 0, (size_t)bytes) && sqlite3_column_type(st, 1) == SQLITE_INTEGER &&
            sqlite3_column_type(st, 2) == SQLITE_INTEGER &&
            sqlite3_column_type(st, 3) == SQLITE_INTEGER && sqlite3_column_type(st, 4) == SQLITE_INTEGER &&
            data->size > 0 && data->size <= MEDIA_BLOCK_SIZE_MAX && data->offset >= 0 && data->offset < data->file_size &&
            data->size <= data->file_size - data->offset;
        if (!valid) rc = SQLITE_CORRUPT;
        else { memcpy(location, value, (size_t)bytes); location[bytes] = 0; data->found = 1; rc = sqlite3_step(st); }
    }
    rc = md_sql_result(db, st, rc, "indexed provider snapshot"); if (rc || !data->found) return rc;
    char base[MEDIA_BASE_MAX];
    rc = media_index_base_read(db, base);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: provider media base rc=%d", MD_ID, rc); return rc;
    }
    return media_index_path(base, location, data->path) ? SQLITE_MISMATCH : SQLITE_OK;
}
static int md_provider_guard(struct md_sql_job* job) {
    struct md_provider_sql* data = (void*)(job + 1);
    return topo_group_chat_key_allowed(job->inst, data->group, data->peer);
}
static void md_provider_done(struct md_sql_job* job, const struct db_async_result* result) {
    struct md_provider_sql* data = (void*)(job + 1); struct media_delivery_ctx* md = &job->inst->md;
    if (!md_provider_guard(job)) return;
    struct file_io_info info;
    if (result->status || !data->found || file_io_inspect(data->path, 1, &info) || info.type != FILE_IO_REGULAR || info.size != data->file_size) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: indexed provider unavailable group=%llx peer=%s found=%d status=%d rc=%d", MD_ID,
            (unsigned long long)data->group, sc_public_key_hex(data->peer, (char[65]){0}), data->found, result->status, result->sqlite_code);
        md_block_reply(md, data->group, data->peer, &data->request, 0); return;
    }
    struct ll_entry* entry = queue_entry_new(sizeof(struct media_block_source));
    if (!entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: indexed provider source allocation failed", MD_ID);
        md_block_reply(md, data->group, data->peer, &data->request, 1); return;
    }
    struct media_block_source* source = (void*)entry->data; memset(source, 0, sizeof(*source));
    source->entry = entry; source->md = md; source->refs = 1; source->verified = 1;
    source->group_id = data->group; source->chunk = data->request.chunk; source->size = source->available = (uint32_t)data->size;
    source->base = (uint64_t)data->offset;
    memcpy(source->block_id, data->request.block_id, 16); memcpy(source->media_id, data->request.media_id, 16);
    memcpy(source->storage.temporary, data->path, strlen(data->path) + 1);
    md_stream_start(md, data->group, data->peer, &data->request, source);
}
static int md_provider_submit(struct media_delivery_ctx* md, uint64_t group, const uint8_t peer[32], const struct media_pkt_block_req* request) {
    for (struct md_sql_job* job = md->sql_jobs; job; job = job->next) {
        if (job->work != md_provider_read || job->group != group || job->cancelled) continue;
        const struct md_provider_sql* data = (const void*)(job + 1);
        if (!memcmp(data->peer, peer, 32) && !memcmp(data->request.transfer_id, request->transfer_id, 16)) {
            if (memcmp(&data->request, request, sizeof(*request))) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: conflicting pending BLOCK_REQ", MD_ID);
            return 0;
        }
    }
    struct md_sql_job* job = md_sql_new(md, group, sizeof(struct md_provider_sql), "media provider", DB_ASYNC_READ, md_provider_read, md_provider_done);
    if (!job) return -1;
    struct md_provider_sql* data = (void*)(job + 1); data->group = group; strcpy(data->channel, topo_groups_channel(job->inst->topo_groups, data->group)); memcpy(data->peer, peer, sizeof(data->peer));
    memcpy(data->self_ed, md->inst->my_ed25519_pubkey, 32);
    data->request = *request; job->guard = md_provider_guard; md_sql_dispatch(job); return 0;
}
static int md_provider_cancel(struct media_delivery_ctx* md, uint64_t group, const uint8_t peer[32], const struct media_pkt_cancel* request) {
    int cancelled = 0;
    for (struct md_sql_job* job = md->sql_jobs, *next; job; job = next) {
        next = job->next;
        if (job->work != md_provider_read || job->group != group) continue;
        const struct md_provider_sql* data = (const void*)(job + 1);
        if (memcmp(data->peer, peer, 32) || data->request.chunk != request->chunk || memcmp(data->request.transfer_id, request->transfer_id, 16) ||
            memcmp(data->request.media_id, request->media_id, 16) || memcmp(data->request.block_id, request->block_id, 16)) continue;
        job->cancelled = 1; cancelled++;
        if (job->retry) md_sql_free(job);
    }
    if (cancelled) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: pending indexed provider cancelled group=%llx peer=%s count=%d", MD_ID,
        (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), cancelled);
    return cancelled;
}

static void md_handle_block_req(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t peer[32],
                                const uint8_t* data, size_t len) {
    if (len != MEDIA_BLOCK_REQ_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: malformed BLOCK_REQ bytes=%zu", MD_ID, len); return;
    }
    const struct media_pkt_block_req* req = (const struct media_pkt_block_req*)data;
    int relay_count = 0;
    struct media_block_source* source = md_block_find(md, group_id, req->media_id, req->block_id);
    if (source && source->chunk != req->chunk) source = NULL;
    for (struct stream_ctx* sc = md->streams; sc; sc = sc->next) {
        if (sc->source == source) relay_count++;
        if (sc->group_id != group_id || memcmp(sc->dst_ed, peer, 32) || memcmp(sc->transfer_id, req->transfer_id, 16)) continue;
        if (sc->source->chunk != req->chunk || memcmp(sc->source->media_id, req->media_id, 16)
            || memcmp(sc->source->block_id, req->block_id, 16))
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: conflicting BLOCK_REQ attempt peer=%s", MD_ID, sc_public_key_hex(peer, (char[65]){0}));
        return;
    }
    struct md_file_load* load = md_file_load_find(md, req->media_id);
    if ((source && relay_count >= MD_MAX_RELAY_DOWNSTREAM)
        || (!source && (md->active_streams >= MD_MAX_STREAMS
            || (load && md->max_downloads_per_file && load->active_downloads >= md->max_downloads_per_file)))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: BLOCK_REQ admission busy peer=%s active=%u file=%d relay=%d",
                   MD_ID, sc_public_key_hex(peer, (char[65]){0}), md->active_streams, load ? load->active_downloads : 0, relay_count);
        uint8_t nodes[MD_MAX_RELAY_DOWNSTREAM][32]; unsigned count = 0;
        for (struct stream_ctx* sc = md->streams; sc && count < MD_MAX_RELAY_DOWNSTREAM; sc = sc->next) {
            if (sc->group_id != group_id || memcmp(sc->source->block_id, req->block_id, 16)
                || memcmp(sc->source->media_id, req->media_id, 16) || sc->source->chunk != req->chunk
                || !memcmp(sc->dst_ed, peer, 32) || !memcmp(sc->dst_ed, md->inst->my_ed25519_pubkey, 32)) continue;
            unsigned i = 0;
            while (i < count && memcmp(nodes[i], sc->dst_ed, 32)) i++;
            if (i == count) memcpy(nodes[count++], sc->dst_ed, sizeof(nodes[0]));
        }
        if (count) {
            uint8_t packet[MEDIA_RELAY_FULL_HDR_SIZE + sizeof(nodes)] = {0};
            struct media_pkt_block_relay_full* full = (struct media_pkt_block_relay_full*)packet;
            full->subcmd = MEDIA_SUBCMD_BLOCK_RELAY_FULL; full->chunk = req->chunk; full->num_relay_nodes = (uint16_t)count;
            memcpy(full->media_id, req->media_id, 16); memcpy(full->block_id, req->block_id, 16);
            memcpy(full->transfer_id, req->transfer_id, 16); memcpy(packet + sizeof(*full), nodes, count * sizeof(nodes[0]));
            md_send_key(md->inst, group_id, peer, packet, sizeof(*full) + count * sizeof(nodes[0]), 1); return;
        }
        md_block_reply(md, group_id, peer, req, 1); return;
    }
    if (source) source->refs++;
    else {
        if (db_async_can_write(md->inst->db_worker)) {
            if (md_provider_submit(md, group_id, peer, req)) md_block_reply(md, group_id, peer, req, 1);
            return;
        }
        sqlite3_stmt* st = NULL;
        const char* channel = topo_groups_channel(md->inst->topo_groups, group_id);
        const char* sql = "SELECT location,chunk_size,chunk,offset,file_size,chat_id FROM media_public_files"
                          " WHERE block_id=? AND media_id=? AND holder=? AND chat_id=? LIMIT 1";
        int rc = sqlite3_prepare_v2(md->db, sql, -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, req->block_id, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, req->media_id, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, md->inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 4, channel, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        char base[MEDIA_BASE_MAX], path[MEDIA_PATH_MAX];
        const char* location = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
        int64_t size = rc == SQLITE_ROW ? sqlite3_column_int64(st, 1) : 0;
        int64_t offset = rc == SQLITE_ROW ? sqlite3_column_int64(st, 3) : -1;
        int64_t file_size = rc == SQLITE_ROW ? sqlite3_column_int64(st, 4) : 0;
        const char* chat = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 5) : NULL;
        int valid = location && sqlite3_column_bytes(st, 0) == (int)strlen(location)
            && chat && sqlite3_column_bytes(st, 5) == (int)strlen(chat) && !strcmp(chat, channel)
            && sqlite3_column_int64(st, 2) == req->chunk && size > 0 && size <= MEDIA_BLOCK_SIZE_MAX
            && offset >= 0 && offset < file_size && size <= file_size - offset
            && !media_index_get_base(md->db, base) && !media_index_path(base, location, path);
        int final_rc = sqlite3_finalize(st);
        struct file_io_info info;
        if (!valid || final_rc != SQLITE_OK || file_io_inspect(path, 1, &info)
            || info.type != FILE_IO_REGULAR || info.size != file_size) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: BLOCK_REQ unavailable peer=%s chunk=%u sql=%d finalize=%d",
                       MD_ID, sc_public_key_hex(peer, (char[65]){0}), req->chunk, rc, final_rc);
            md_block_reply(md, group_id, peer, req, 0); return;
        }
        struct ll_entry* entry = queue_entry_new(sizeof(*source));
        if (!entry) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: indexed source allocation failed", MD_ID);
            md_block_reply(md, group_id, peer, req, 0); return;
        }
        source = (struct media_block_source*)entry->data; memset(source, 0, sizeof(*source));
        source->entry = entry; source->md = md; source->refs = 1; source->verified = 1;
        source->group_id = group_id; source->chunk = req->chunk; source->size = source->available = (uint32_t)size;
        source->base = (uint64_t)offset;
        memcpy(source->block_id, req->block_id, 16); memcpy(source->media_id, req->media_id, 16);
        memcpy(source->storage.temporary, path, strlen(path) + 1);
    }
    md_stream_start(md, group_id, peer, req, source);
}
static void md_handle_cancel(struct media_delivery_ctx* md, uint64_t group, const uint8_t peer[32],
                             const uint8_t* data, size_t len) {
    if (len != sizeof(struct media_pkt_cancel)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: malformed CANCEL bytes=%zu", MD_ID, len); return;
    }
    const struct media_pkt_cancel* can = (const struct media_pkt_cancel*)data;
    int pending = md_provider_cancel(md, group, peer, can);
    for (struct stream_ctx* sc = md->streams; sc; sc = sc->next) {
        if (sc->group_id == group && !memcmp(sc->dst_ed, peer, 32) && sc->source->chunk == can->chunk
            && !memcmp(sc->transfer_id, can->transfer_id, 16) && !memcmp(sc->source->media_id, can->media_id, 16)
            && !memcmp(sc->source->block_id, can->block_id, 16)) {
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: TX cancelled peer=%s chunk=%u", MD_ID,
                       sc_public_key_hex(peer, (char[65]){0}), can->chunk);
            stream_free(sc); return;
        }
    }
    if (!pending) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: stale CANCEL peer=%s chunk=%u", MD_ID, sc_public_key_hex(peer, (char[65]){0}), can->chunk);
}

/* ── decrement stream counter (called when stream finished/cancelled internally) ── */

void media_delivery_stream_done(struct UTUN_INSTANCE* inst) {
    if (inst) { struct media_delivery_ctx* md = &inst->md; if (md->active_streams > 0) md->active_streams--; md->stream_completed = 1; }
}

static void md_super_connect(struct media_delivery_ctx* md, const uint8_t ed[32], uint64_t group_id) {
    if (!memcmp(ed, md->inst->my_ed25519_pubkey, 32)) return;
    struct media_super_peer* peer = md_super_peer_add(md, ed, group_id);
    if (!peer) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot allocate replication peer", MD_ID); return; }
    if (peer->connection || peer->connected || get_time_tb() < peer->deadline_tb) return;

    if (topo_group_chat_key_allowed(md->inst, group_id, ed) &&
        !conn_mgr_open(md->inst, group_id, ed, md_super_conn_cb, peer, &peer->connection)) return;
    if (etcp_router_peer_reachable(md->inst, group_id, ed, ETCP_RT_ID_MEDIA_DELIVERY)) {
        peer->connected = 1;
        if (md_repl_request(peer)) peer->deadline_tb = get_time_tb() + peer->timeout_tb;
        md_repl_schedule(peer);
        return;
    }
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: replication path unavailable group=%llx peer=%s", MD_ID,
        (unsigned long long)group_id, sc_public_key_hex(ed, (char[65]){0}));
    peer->deadline_tb = get_time_tb() + peer->timeout_tb;
}

/* Подписанные adm_tags — источник роли; node_type/deleted являются производными кешами. */
static int md_tags_super(const char* tags) {
    char value[16], deleted[16];
    return tags && json_flat_get(tags, "supernode", value, sizeof(value)) == 0 && !strcmp(value, "yes")
        && !(json_flat_get(tags, "deleted", deleted, sizeof(deleted)) == 0 && !strcmp(deleted, "yes"));
}

/* 1/0 — актуальная роль в существующей CHAT-группе; -1 — ошибка SQL, не повод удалять кеш. */
static int md_group_role(struct media_delivery_ctx* md, uint64_t group_id, const uint8_t ed[32]) {
    struct TOPO_GROUP* group = md->inst->topo_groups ? topo_groups_find(md->inst->topo_groups, group_id) : NULL;
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT || group->stopping || !group->channel_id[0]) return 0;
    return group_policy_member_role_key(md->inst, group->channel_id, ed, GROUP_MEMBER_SUPERNODE);
}

/* Удалённые данные и RX-позиции всегда очищаются вместе; собственные объявления сохраняются при потере роли. */
static int md_purge_group(struct media_delivery_ctx* md, uint64_t group_id, int all) {
    if (db_async_can_write(md->inst->db_worker)) return md_role_submit(md, group_id, 0, all);
    if (md_sql_begin(md->db, "BEGIN IMMEDIATE")) return -1;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(md->db,
        "DELETE FROM block_availability WHERE group_id=? AND (? OR holder!=?)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, topo_groups_channel(md->inst->topo_groups, group_id), -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 2, all);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, md->inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (md_sql_finish(md->db, st, rc, "purge group rows")) goto failed;
    int removed = sqlite3_changes(md->db);
    st = NULL;
    rc = sqlite3_prepare_v2(md->db, "DELETE FROM super_sync WHERE group_id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, topo_groups_channel(md->inst->topo_groups, group_id), -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (md_sql_finish(md->db, st, rc, "purge group cursors") || md_sql_exec(md->db, "COMMIT")) goto failed;
    if (removed) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: group cache reset group=%llx rows=%d all=%d",
                           MD_ID, (unsigned long long)group_id, removed, all);
    return 0;
failed:
    md_sql_rollback(md->db); return -1;
}

/* Единственный суперузел тоже должен реплицировать собственные зарегистрированные блоки.
 * Стабильный timestamp из media_files исключает новые ID при каждом перечитывании ролей. */
static int md_seed_local(struct media_delivery_ctx* md, uint64_t group_id) {
    if (db_async_can_write(md->inst->db_worker)) return md_role_submit(md, group_id, 1, 0);
    const char* channel = topo_groups_channel(md->inst->topo_groups, group_id);
    if (md_sql_begin(md->db, "BEGIN IMMEDIATE")) return -1;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(md->db, "SELECT block_id,chunk,timestamp,media_id FROM media_public_files WHERE chat_id=? AND holder=?",
                               -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, md->inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    int changed = 0;
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 chunk = sqlite3_column_int64(st, 1), ts = sqlite3_column_int64(st, 2);
        if (sqlite3_column_bytes(st, 0) != 16 || chunk < 0 || chunk > UINT32_MAX || ts < 0) { rc = SQLITE_CORRUPT; break; }
        struct media_pkt_sync_entry row;
        int applied = md_holder_own(md, group_id, sqlite3_column_blob(st, 3), sqlite3_column_blob(st, 0), (uint32_t)chunk, ts, 1, &row);
        if (applied < 0) { rc = SQLITE_ERROR; break; }
        changed += applied;
    }
    if (md_sql_finish(md->db, st, rc, "seed own blocks") || md_sql_exec(md->db, "COMMIT")) {
        md_sql_rollback(md->db); return -1;
    }
    if (changed) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: own blocks seeded group=%llx changed=%d", MD_ID,
                    (unsigned long long)group_id, changed);
        md_repl_notify(md, group_id);
    }
    return 0;
}

/* Согласование идемпотентно, в том числе при новой группе после init. Ошибка роли сохраняет прежний контекст. */
void media_delivery_refresh_roles(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->md.initialized || !inst->topo_groups) return;
    struct media_delivery_ctx* md = &inst->md;
    int rw = db_async_can_write(inst->db_worker);
    if (!rw && (!md->db || !sqlite3_get_autocommit(md->db))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: role reconciliation deferred during SQL transaction", MD_ID); return;
    }
    for (struct ll_entry* e = md->super_peers ? md->super_peers->head : NULL; e;) {
        struct media_super_peer* peer = (struct media_super_peer*)e->data;
        e = e->next;
        int own = md_group_role(md, peer->group_id, md->inst->my_ed25519_pubkey);
        int other = md_group_role(md, peer->group_id, peer->peer_ed);
        if (own == 0 || other == 0) md_super_peer_remove(md, peer->peer_ed, peer->group_id);
    }
    int any = 0;
    for (struct ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (group->group_type != TOPO_GROUP_TYPE_CHAT || group->stopping) continue;
        int own = md_group_role(md, group->group_id, inst->my_ed25519_pubkey);
        if (own < 0) { any |= md->is_supernode; continue; }
        if (!own) { md_purge_group(md, group->group_id, 0); continue; }
        any = 1;
        md_seed_local(md, group->group_id);
        if (rw) {
            size_t cursor = 0; uint8_t ed[32];
            while (group_policy_role_key_next(inst, group->channel_id, GROUP_MEMBER_SUPERNODE, &cursor, ed))
                md_super_connect(md, ed, group->group_id);
            continue;
        }
        char sql[256];
        sqlite3_snprintf(sizeof(sql), sql,
            "SELECT ed25519_pubkey,adm_tags FROM \"peers_%w\" WHERE ed25519_pubkey!=?"
            " AND record_state=0 AND source=0 AND identity_verified=1 AND deleted=0 AND admitted=1", group->channel_id);
        sqlite3_stmt* st = NULL;
        int rc = sqlite3_prepare_v2(md->db, sql, -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const uint8_t* ed = sqlite3_column_blob(st, 0);
            if (ed && sqlite3_column_bytes(st, 0) == 32 && md_tags_super((const char*)sqlite3_column_text(st, 1)))
                md_super_connect(md, ed, group->group_id);
        }
        md_sql_finish(md->db, st, rc, "enumerate supernodes");
    }
    if (md->is_supernode != any) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: aggregate supernode role=%d", MD_ID, any);
    md->is_supernode = (uint8_t)any;
}

static void md_reconcile_timer(void* arg) {
    struct media_delivery_ctx* md = arg;
    md->reconcile_timer = NULL;
    md_sql_retry(md);
    media_delivery_refresh_roles(md->inst);
    md->reconcile_timer = uasync_set_timeout(md->inst->ua, MEDIA_REPL_POLL_TB, md, md_reconcile_timer, "md_roles");
    if (!md->reconcile_timer) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: role reconciliation timer allocation failed", MD_ID);
}

/* member_sync сообщает и изменение подписанных свойств, и BGP-события; REMOVE маршрута не удаляет участника. */
static void md_on_props_changed(const uint8_t ed[32], const char* channel_id, void* arg) {
    struct media_delivery_ctx* md = arg;
    if (!md->initialized) return;
    uint64_t group = topo_groups_channel_slot(md->inst->topo_groups, channel_id);
    if (!group) return;
    if (!db_async_can_write(md->inst->db_worker) && (!md->db || !sqlite3_get_autocommit(md->db))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: access reconciliation deferred during SQL transaction", MD_ID); return;
    }
    etcp_router_revalidate_group(md->inst, group);
    (void)ed;
    if (!topo_group_chat_key_allowed(md->inst, group, md->inst->my_ed25519_pubkey))
        media_delivery_cancel_channel(md->inst, group);
    media_delivery_refresh_roles(md->inst);
}

static void md_on_conn_status(struct ETCP_CONN* conn, int status, void* arg) {
    struct media_delivery_ctx* md = (struct media_delivery_ctx*)arg;
    if (!conn || !md->initialized) return;
    switch (status) {
    case ETCP_CONN_STATUS_DOWN:
    case ETCP_CONN_STATUS_DELETE:
        for (struct ll_entry* e = md->served_nodes ? md->served_nodes->head : NULL; e;) {
            struct ll_entry* next = e->next;
            if (!memcmp(((struct media_served_node*)e->data)->peer_ed, conn->peer_ed25519_pubkey, 32)) {
                queue_remove_data(md->served_nodes, e); queue_entry_free(e);
            }
            e = next;
        }
        break;
    default: break;
    }
}

/* ── main dispatch ── */

static void md_etcp_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!conn || !entry || !entry->dgram || entry->len < 1) {
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }

    struct UTUN_INSTANCE* inst = conn->instance;
    if (!inst) { queue_dgram_free(entry); queue_entry_free(entry); return; }
    struct media_delivery_ctx* md = &inst->md;
    if (!md->initialized) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: recv before init", MD_ID); queue_dgram_free(entry); queue_entry_free(entry); return; }

    const uint8_t* data = entry->dgram;
    size_t len = entry->len;
    if (len < ROUTER_SVC_PAYLOAD_OFF) { queue_dgram_free(entry); queue_entry_free(entry); return; }
    uint64_t group_id;
    memcpy(&group_id, data + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* from_ed = data + ROUTER_SVC_SRC_ED_OFF;
    if (len == ROUTER_SVC_PAYLOAD_OFF) {
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: channel closed group=%llx peer=%s", MD_ID,
                   (unsigned long long)group_id, sc_public_key_hex(from_ed, (char[65]){0}));
        uint8_t key[40]; md_peer_key(key, group_id, from_ed);
        struct ll_entry* served = md->served_nodes ? queue_find_data_by_index(md->served_nodes, key) : NULL;
        if (served) { queue_remove_data(md->served_nodes, served); queue_entry_free(served); }
        for (struct stream_ctx* sc = md->streams; sc;) {
            struct stream_ctx* next = sc->next;
            if (sc->group_id == group_id && !memcmp(sc->dst_ed, from_ed, 32)) stream_free(sc);
            sc = next;
        }
        media_download_peer_closed(inst, group_id, from_ed);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    uint8_t subcmd = data[ROUTER_SVC_PAYLOAD_OFF];
    data += ROUTER_SVC_PAYLOAD_OFF; len -= ROUTER_SVC_PAYLOAD_OFF;

    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: recv %s(%02x) from %s len=%zu",
                MD_ID, md_subcmd_name(subcmd), subcmd, sc_public_key_hex(from_ed, (char[65]){0}), len);

    if (subcmd == MEDIA_SUBCMD_SYNC_REQ || subcmd == MEDIA_SUBCMD_SYNC_RESP || subcmd == MEDIA_SUBCMD_CHANGED) {
        if (!memcmp(from_ed, md->inst->my_ed25519_pubkey, 32) || md_group_role(md, group_id, md->inst->my_ed25519_pubkey) != 1
            || md_group_role(md, group_id, from_ed) != 1) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: replication role rejected group=%llx peer=%s cmd=%u", MD_ID,
                       (unsigned long long)group_id, sc_public_key_hex(from_ed, (char[65]){0}), subcmd);
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
    }
    switch (subcmd) {
    case MEDIA_SUBCMD_SERVE_REG:
        md_handle_serve_reg(md, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_SERVE_LEAVE: {
        const uint8_t* group_ed = md_group_key(inst, group_id);
        if (len != sizeof(struct media_pkt_serve_leave) || !group_ed ||
            memcmp(((const struct media_pkt_serve_leave*)data)->group_ed, group_ed, 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: SERVE_LEAVE invalid size/group bytes=%zu group=%llx", MD_ID, len,
                       (unsigned long long)group_id); break;
        }
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: SERVE_LEAVE from %s",
                   MD_ID, sc_public_key_hex(from_ed, (char[65]){0}));
        if (md->served_nodes) {
            uint8_t key[40]; md_peer_key(key, group_id, from_ed);
            struct ll_entry* e = queue_find_data_by_index(md->served_nodes, key);
            if (e) { queue_remove_data(md->served_nodes, e); queue_entry_free(e); }
        }
        break;
    }
    case MEDIA_SUBCMD_QUERY:
        md_handle_query(md, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_HAVE_BLOCK:
        md_handle_announcement(md, group_id, from_ed, data, len, 1);
        break;
    case MEDIA_SUBCMD_BLOCK_PROCESSING:
        md_handle_announcement(md, group_id, from_ed, data, len, 0);
        break;
    case MEDIA_SUBCMD_SYNC_REQ:
        md_handle_sync_req(md, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_SYNC_RESP:
        md_handle_sync_resp(md, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_CHANGED: {
        if (len != 1) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: malformed CHANGED bytes=%zu", MD_ID, len); break; }
        struct media_super_peer* peer = md_super_peer_find(md, from_ed, group_id);
        if (!peer) { md_super_connect(md, from_ed, group_id); peer = md_super_peer_find(md, from_ed, group_id); }
        if (peer) {
            if (peer->request_pending) peer->dirty = 1;
            else peer->deadline_tb = 0;
        }
        break;
    }
    case MEDIA_SUBCMD_QUERY_RESP:
        media_download_handle_query_resp(inst, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_HAVE_BLOCK_ACK:
        /* handled by download module */
        break;
    case MEDIA_SUBCMD_SERVE_ACK:
        break;
    case MEDIA_SUBCMD_BLOCK_REQ:
        md_handle_block_req(md, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_BLOCK_OVERLOADED:
        media_download_handle_overloaded(inst, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_BLOCK_RELAY_FULL:
        media_download_handle_relay_full(inst, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_BLOCK_CHUNK:
        media_download_handle_chunk(inst, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_BLOCK_DONE:
        media_download_handle_done(inst, group_id, from_ed, data, len);
        break;
    case MEDIA_SUBCMD_CANCEL:
        md_handle_cancel(md, group_id, from_ed, data, len);
        break;
    default:
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: unknown subcmd 0x%02x from %s",
                   MD_ID, subcmd, sc_public_key_hex(from_ed, (char[65]){0}));
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

/* ── table creation ── */

/* Каталог создаётся только в актуальном формате; старые данные не сбрасываются. */
int media_delivery_prepare_write(sqlite3* db, uint8_t output[16]) {
    if (!db || !output || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: prepare requires caller transaction", MD_ID); return SQLITE_MISUSE;
    }
    const char* names[] = {"media_replication_meta", "block_availability", "super_sync"};
    for (unsigned i = 0; i < 3; i++) if (db_schema_table(db, names[i], NULL)) {
        int error = sqlite3_extended_errcode(db); return error ? error : SQLITE_ERROR;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT version,source_epoch FROM media_replication_meta WHERE singleton=1", -1, &st, NULL);
    uint8_t epoch[16] = {0}; int created = 0;
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_int(st, 0) != 3 || sqlite3_column_bytes(st, 1) != 16) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: incompatible catalog metadata; startup stopped without resetting data", MD_ID);
            sqlite3_finalize(st); return SQLITE_SCHEMA;
        }
        memcpy(epoch, sqlite3_column_blob(st, 1), 16); rc = sqlite3_step(st);
    } else if (rc == SQLITE_DONE) created = 1;
    rc = md_sql_result(db, st, rc, "catalog metadata"); if (rc) return rc;
    if (created) {
        if (media_index_generate_uuid(epoch)) return SQLITE_ERROR;
        st = NULL;
        rc = sqlite3_prepare_v2(db, "INSERT INTO media_replication_meta VALUES(1,3,?)", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, epoch, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        rc = md_sql_result(db, st, rc, "write catalog epoch"); if (rc) return rc;
    }
    memcpy(output, epoch, 16);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: catalog initialized new=%d epoch=%02x%02x", MD_ID, created, epoch[0], epoch[1]);
    return SQLITE_OK;
}

/* ── public API ── */

int media_delivery_init(struct UTUN_INSTANCE* inst) {
    if (!inst || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: init needs prepared RW activation", MD_ID); return -1;
    }
    if (inst->md.inst) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: init requires destroyed previous runtime", MD_ID); return -1; }
    inst->md.db = inst->topo_sqlite_db;
    sqlite3* db = inst->topo_sqlite_db; uint8_t epoch[16];
    if (md_sql_begin(db, "BEGIN IMMEDIATE")) return -1;
    if (media_delivery_prepare_write(db, epoch) || md_sql_exec(db, "COMMIT")) { md_sql_rollback(db); return -1; }
    return media_delivery_activate(inst, epoch);
}

int media_delivery_activate(struct UTUN_INSTANCE* inst, const uint8_t epoch[16]) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: init with NULL inst", MD_ID); return -1; }
    struct media_delivery_ctx* md = &inst->md;
    if (!epoch || md->initialized || md->inst) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: activation requires fresh runtime and prepared epoch", MD_ID); return -1;
    }
    memset(md, 0, sizeof(*md));
    md->inst = inst;
    md->db = inst->topo_sqlite_db;
    md->max_downloads_per_file = 3;
    md->dl_stall_timeout_tb = MEDIA_DL_STALL_TIMEOUT_TB;
    md->dl_max_attempts = MEDIA_DL_MAX_ATTEMPTS;
    memcpy(md->source_epoch, epoch, 16);

    if (etcp_router_bind(inst, ETCP_RT_ID_MEDIA_DELIVERY, md_etcp_recv) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: failed to bind ETCP_RT_ID_MEDIA_DELIVERY", MD_ID);
        memset(md, 0, sizeof(*md)); return -1;
    }
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_MEDIA_DELIVERY, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_MEDIA_DELIVERY, topo_group_chat_key_allowed)) {
        etcp_router_unbind(inst, ETCP_RT_ID_MEDIA_DELIVERY); memset(md, 0, sizeof(*md)); return -1;
    }

    if (member_sync_add_props_cbk(inst, md_on_props_changed, md)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot subscribe to member roles", MD_ID);
        etcp_router_unbind(inst, ETCP_RT_ID_MEDIA_DELIVERY); memset(md, 0, sizeof(*md)); return -1;
    }
    etcp_add_conn_status_cbk(inst, md_on_conn_status, md);

    md->initialized = 1;
    media_delivery_refresh_roles(inst);
    md->reconcile_timer = uasync_set_timeout(inst->ua, MEDIA_REPL_POLL_TB, md, md_reconcile_timer, "md_roles");
    if (!md->reconcile_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: role reconciliation timer allocation failed", MD_ID);
        media_delivery_destroy(inst); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: initialized holder=%s super=%d",
               MD_ID, sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), md->is_supernode);
    return 0;
}

void media_delivery_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->md.inst) return;
    struct media_delivery_ctx* md = &inst->md;
    media_delivery_quiesce(inst);
    while (md->streams) stream_free(md->streams);

    etcp_router_unbind(inst, ETCP_RT_ID_MEDIA_DELIVERY);
    etcp_remove_conn_status_cbk(inst, md_on_conn_status, md);
    member_sync_remove_props_cbk(inst, md_on_props_changed, md);

    if (md->super_peers) {
        while (md->super_peers->head) {
            struct media_super_peer* peer = (struct media_super_peer*)md->super_peers->head->data;
            md_super_peer_remove(md, peer->peer_ed, peer->group_id);
        }
        queue_free(md->super_peers);
        md->super_peers = NULL;
    }
    if (md->served_nodes) { queue_free(md->served_nodes); md->served_nodes = NULL; }
    if (md->downloads) {
        while (md->downloads->head) {
            struct media_download* dl = (struct media_download*)md->downloads->head->data;
            media_download_cancel_group_object(inst, dl->group_id, dl->media_id);
        }
        queue_free(md->downloads); md->downloads = NULL;
    }
    if (md->block_sources) { queue_free(md->block_sources); md->block_sources = NULL; }
    if (md->file_loads)   { queue_free(md->file_loads);   md->file_loads   = NULL; }

    while (md->destinations) {
        struct media_destination* destination = md->destinations; md->destinations = destination->next;
        destination->owner = NULL; destination->next = NULL;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: detached pending SQL destination=%s", MD_ID, destination->path);
    }
    memset(md, 0, sizeof(*md));
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: destroyed", MD_ID);
}

void media_delivery_quiesce(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->md.inst) return;
    struct media_delivery_ctx* md = &inst->md;
    md->initialized = 0; md_sql_detach(md);
    if (md->reconcile_timer) { uasync_cancel_timeout(inst->ua, md->reconcile_timer); md->reconcile_timer = NULL; }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: SQL producers quiesced before media drain", MD_ID);
}

void media_delivery_cancel_channel(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    if (!inst || !inst->md.initialized) return;
    struct media_delivery_ctx* md = &inst->md;

    struct stream_ctx* stream = md->streams;
    while (stream) {
        struct stream_ctx* next = stream->next;
        if (stream->group_id == group_id) stream_free(stream);
        stream = next;
    }
    /* Callback отмены может изменить остальные загрузки: каждый раз ищем следующую заново. */
    int nm = 0;
    for (;;) {
        struct media_download* found = NULL;
        for (struct ll_entry* e = md->downloads ? md->downloads->head : NULL; e; e = e->next) {
            struct media_download* dl = (struct media_download*)e->data;
            if (dl->group_id == group_id) { found = dl; break; }
        }
        if (!found) break;
        media_download_cancel_group_object(inst, group_id, found->media_id); nm++;
    }

    /* 3. удалить super_peers этой группы */
    if (md->super_peers) {
        struct ll_entry* se = md->super_peers->head;
        while (se) {
            struct media_super_peer* sp = (struct media_super_peer*)se->data;
            struct ll_entry* next = se->next;
            if (sp->group_id == group_id)
                md_super_peer_remove(md, sp->peer_ed, group_id);
            se = next;
        }
    }

    md_purge_group(md, group_id, 1);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: cancel_channel group=%016llx downloads=%d",
               MD_ID, (unsigned long long)group_id, nm);
}

int media_delivery_bind(struct UTUN_INSTANCE* inst) {
    return media_delivery_init(inst);
}

/* Адресаты только этой группы, роли читаем из подписанных adm_tags, не из node_type. */
static int md_collect_supernodes(struct UTUN_INSTANCE* inst, uint64_t group_id, uint8_t supers[10][32]) {
    struct TOPO_GROUP* g = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (!g || g->group_type != TOPO_GROUP_TYPE_CHAT || g->stopping || !g->channel_id[0]) return 0;
    if (db_async_can_write(inst->db_worker)) {
        size_t cursor = 0; int count = 0; uint8_t ed[32];
        while (count < 10 && group_policy_role_key_next(inst, g->channel_id, GROUP_MEMBER_SUPERNODE, &cursor, ed))
            if (memcmp(ed, inst->my_ed25519_pubkey, 32)) memcpy(supers[count++], ed, 32);
        return count;
    }
    char sql[256];
    sqlite3_snprintf(sizeof(sql), sql, "SELECT ed25519_pubkey,adm_tags FROM \"peers_%w\" WHERE ed25519_pubkey!=?"
        " AND record_state=0 AND source=0 AND identity_verified=1 AND deleted=0 AND admitted=1 ORDER BY ed25519_pubkey", g->channel_id);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(inst->md.db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    int count = 0;
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t* ed = sqlite3_column_blob(st, 0);
        if (ed && sqlite3_column_bytes(st, 0) == 32 && md_tags_super((const char*)sqlite3_column_text(st, 1)) && count < 10)
            memcpy(supers[count++], ed, 32);
    }
    return md_sql_finish(inst->md.db, st, rc, "announcement recipients") ? 0 : count;
}

static void md_send_own_holder(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t peer[32],
    const uint8_t* media_id, const uint8_t* block_id, uint32_t chunk, int status) {
    if (!inst || !peer || !media_id || !block_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: send_have_block invalid args inst=%p media=%p block=%p",
                    MD_ID, (void*)inst, (void*)media_id, (void*)block_id);
        return;
    }
    struct media_pkt_have_block hb;
    memset(&hb, 0, sizeof(hb));
    hb.subcmd = MEDIA_SUBCMD_HAVE_BLOCK;
    const uint8_t* group_ed = md_group_key(inst, group_id);
    if (!group_ed) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: announcement group key unavailable group=%llx", MD_ID, (unsigned long long)group_id); return; }
    memcpy(hb.group_ed, group_ed, 32);
    memcpy(hb.media_id, media_id, 16);
    memcpy(hb.block_id, block_id, 16);
    hb.chunk = chunk;
    hb.timestamp = (int64_t)time(NULL);
    if (db_async_can_write(inst->db_worker)) { md_announce_submit(&inst->md, group_id, peer, &hb, NULL, status); return; }
    /* Зарегистрированный блок переанонсируется с прежним временем, не создавая новое событие. */
    sqlite3_stmt* st = NULL;
    const char* channel = topo_groups_channel(inst->topo_groups, group_id);
    int rc = sqlite3_prepare_v2(inst->md.db, "SELECT timestamp FROM media_public_files"
        " WHERE chat_id=? AND media_id=? AND block_id=? AND holder=? AND chunk=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, media_id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, block_id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 5, chunk);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) { hb.timestamp = sqlite3_column_int64(st, 0); rc = sqlite3_step(st); }
    }
    if (md_sql_finish(inst->md.db, st, rc, "read own announcement timestamp")) return;

    if (!inst->md.initialized || md_sql_begin(inst->md.db, "BEGIN IMMEDIATE")) return;
    struct media_pkt_sync_entry row;
    int changed = md_holder_own(&inst->md, group_id, media_id, block_id, chunk, hb.timestamp, status, &row);
    if (changed < 0 || md_sql_exec(inst->md.db, "COMMIT")) { md_sql_rollback(inst->md.db); return; }
    hb.subcmd = row.status ? MEDIA_SUBCMD_HAVE_BLOCK : MEDIA_SUBCMD_BLOCK_PROCESSING;
    hb.chunk = (uint32_t)md_read_le(&row.chunk, 4); hb.timestamp = (int64_t)md_read_le(&row.timestamp, 8);
    memcpy(&hb.version, &row.version, 8); memcpy(hb.signature, row.signature, 64);
    if (changed) md_repl_notify(&inst->md, group_id);
    md_send_key(inst, group_id, peer, (const uint8_t*)&hb, sizeof(hb), 1);
}

void media_delivery_send_have_block(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
    const uint8_t* media, const uint8_t* block, uint32_t chunk) {
    md_send_own_holder(inst, group, peer, media, block, chunk, 1);
}

void media_delivery_send_processing(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
    const uint8_t media[16], const uint8_t block[16], uint32_t chunk) {
    md_send_own_holder(inst, group, peer, media, block, chunk, 0);
}

void media_delivery_announce_media(struct UTUN_INSTANCE* inst, uint64_t group_id,
                                   const uint8_t* media_id, const uint8_t* block_ids,
                                   int num_blocks) {
    if (!inst || !media_id || !block_ids || num_blocks <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: announce invalid args inst=%p media=%p blocks=%p n=%d",
                    MD_ID, (void*)inst, (void*)media_id, (void*)block_ids, num_blocks);
        return;
    }
    struct media_delivery_ctx* md = &inst->md;
    if (!md->initialized) return;

    md_seed_local(md, group_id);
    uint8_t supers[10][32]; int nsup = md_collect_supernodes(inst, group_id, supers);
    if (nsup == 0) {
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: announce — no supernodes for group 0x%016llx",
                   MD_ID, (unsigned long long)group_id);
        return;
    }

    for (int s = 0; s < nsup; s++)
        for (int n = 0; n < num_blocks; n++)
            media_delivery_send_have_block(inst, group_id, supers[s], media_id,
                                           block_ids + (size_t)n * 16, (uint32_t)n);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: announce media=%02x%02x... blocks=%d supers=%d",
               MD_ID, media_id[0], media_id[1], num_blocks, nsup);
}

void media_delivery_announce_local_blocks(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    struct media_delivery_ctx* md = &inst->md;
    if (!md->initialized) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: announce_local_blocks — media_delivery not initialized", MD_ID); return; }
    if (db_async_can_write(inst->db_worker)) { media_delivery_refresh_roles(inst); md_local_submit(md); return; }
    sqlite3* db = inst->topo_sqlite_db;
    if (!db) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: announce_local_blocks — no DB", MD_ID); return; }

    media_delivery_refresh_roles(inst);

    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT id,media_id,block_id,chunk,chat_id FROM media_public_files WHERE holder=?", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: announce_local_blocks prepare failed rc=%d: %s", MD_ID, rc, sqlite3_errmsg(db));
        return;
    }
    rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);

    uint64_t rows = 0, eligible = 0, skipped = 0;
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        rows++;
        sqlite3_int64 row = sqlite3_column_int64(st, 0);
        int media_type = sqlite3_column_type(st, 1), block_type = sqlite3_column_type(st, 2);
        int chunk_type = sqlite3_column_type(st, 3), chat_type = sqlite3_column_type(st, 4);
        sqlite3_int64 chunk = sqlite3_column_int64(st, 3);
        int media_bytes = sqlite3_column_bytes(st, 1), block_bytes = sqlite3_column_bytes(st, 2);
        int chat_bytes = sqlite3_column_bytes(st, 4);
        const uint8_t* media = media_type == SQLITE_BLOB && media_bytes == 16 ? sqlite3_column_blob(st, 1) : NULL;
        const uint8_t* block = block_type == SQLITE_BLOB && block_bytes == 16 ? sqlite3_column_blob(st, 2) : NULL;
        const char* chat = chat_type == SQLITE_TEXT ? (const char*)sqlite3_column_text(st, 4) : NULL;
        int valid_chat = chat_bytes == 64 && chat_channel_valid(chat);
        uint64_t gid = valid_chat ? topo_groups_channel_slot(inst->topo_groups, chat) : 0;
        const char* invalid = !media ? "media_id" : !block ? "block_id"
            : chunk_type != SQLITE_INTEGER || chunk < 0 || chunk > UINT32_MAX ? "chunk" : !valid_chat ? "chat_id" : NULL;
        if (invalid) {
            skipped++;
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA,
                       "%s: invalid local block row=%lld field=%s media_type=%d media_bytes=%d block_type=%d block_bytes=%d "
                       "chunk_type=%d chunk=%lld chat_type=%d chat_bytes=%d", MD_ID, (long long)row, invalid,
                       media_type, media_bytes, block_type, block_bytes, chunk_type, (long long)chunk, chat_type, chat_bytes);
            continue;
        }
        uint8_t media_id[16], block_id[16];
        memcpy(media_id, media, sizeof(media_id)); memcpy(block_id, block, sizeof(block_id));
        uint8_t supers[10][32]; int nsup = md_collect_supernodes(inst, gid, supers);
        for (int s = 0; s < nsup; s++)
            media_delivery_send_have_block(inst, gid, supers[s], media_id, block_id, (uint32_t)chunk);
        eligible++;
    }
    if (md_sql_finish(db, st, rc, "enumerate local blocks")) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: local block scan incomplete rows=%llu eligible=%llu skipped=%llu",
                    MD_ID, (unsigned long long)rows, (unsigned long long)eligible, (unsigned long long)skipped);
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: local block scan complete rows=%llu eligible=%llu skipped=%llu",
               MD_ID, (unsigned long long)rows, (unsigned long long)eligible, (unsigned long long)skipped);
}
