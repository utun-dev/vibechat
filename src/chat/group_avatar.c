#include "group_avatar.h"
#include "../db_changes.h"
#include "../db_schema.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "group_policy.h"
#include "member_sync.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../member_files/member_files.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/etcp_router.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/db_async.h"
#include <openssl/sha.h>
#include <openssl/crypto.h>

#define GA_STATE 1
#define GA_GET 2
#define GA_DATA 3
#define GA_WIRE_VERSION 1
#define GA_STATE_BYTES 44
#define GA_DATA_HEADER 112
#define GA_STREAMS 16
#define GA_RETRY_TB 150000ULL
#define GA_EXCHANGE_TB 300000ULL

struct ga_peer {
    struct ga_peer* next;
    struct group_avatar* owner;
    uint64_t group, epoch, advertised_at, requested_at, rx_at, tx_at;
    uint8_t ed[32];
    uint64_t remote_version;
    uint8_t remote_hash[32];
    int state_pending, get_pending, reply_pending, seen;
    uint16_t tx_offset, rx_offset;
    struct group_avatar_record *tx, *rx;
    struct queue_waiter_handle waiter;
    void* pump;
};
struct ga_cache {
    struct ga_cache* next; uint64_t group, registration; unsigned loading; int ready, can_edit;
    struct group_avatar_record value;
};
struct group_avatar {
    struct UTUN_INSTANCE* inst; struct ga_peer* peers; struct ga_cache* cache;
    void* timer; unsigned tx_count, rx_count; int closing;
};

static void ga_pump(void* arg);
static void ga_schedule(struct ga_peer* peer);
static struct group_avatar* ga_context(struct UTUN_INSTANCE* inst) { return inst && CC(inst) ? CC(inst)->group_avatar : NULL; }
static struct ga_cache* ga_cached(struct group_avatar* ctx, uint64_t group, uint64_t registration, int create) {
    for (struct ga_cache* cached = ctx->cache; cached; cached = cached->next) if (cached->group == group) {
        if (cached->registration != registration) {
            cached->registration = registration; cached->ready = cached->can_edit = 0; cached->loading = 0;
            memset(&cached->value, 0, sizeof(cached->value));
        }
        return cached;
    }
    if (!create) return NULL;
    struct ga_cache* cached = u_calloc(1, sizeof(*cached));
    if (!cached) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: cache allocation group=%llu", (unsigned long long)group); return NULL; }
    cached->group = group; cached->registration = registration; cached->next = ctx->cache; ctx->cache = cached; return cached;
}

static int ga_message(const struct group_avatar_record* record, uint8_t* bytes) {
    const char domain[] = "group-avatar-v2"; size_t off = sizeof(domain) - 1;
    memcpy(bytes, domain, off);
    mf_put64(bytes + off, record->version); off += 8;
    bytes[off++] = (uint8_t)record->size; bytes[off++] = (uint8_t)(record->size >> 8);
    memcpy(bytes + off, record->data, record->size); return (int)(off + record->size);
}

static int ga_verify_read(sqlite3* db, const char* channel, const struct group_avatar_record* r) {
    if (!db || !chat_channel_valid(channel) || !r ||
        !r->version || r->version > INT64_MAX || r->size > GROUP_AVATAR_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: invalid record group=%s", channel ? channel : "-"); return SQLITE_MISMATCH;
    }
    uint8_t hash[32], pub[32], bytes[GROUP_AVATAR_MAX + 64];
    SHA256(r->data, r->size, hash);
    if (memcmp(hash, r->hash, 32)) {
        db_changes_warn_hash(db, NULL, channel, "Group avatar", "content hash mismatch", NULL, r->signature, r->hash, hash);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: hash mismatch group=%s version=%llu bytes=%u",
            channel ? channel : "-", (unsigned long long)r->version, r->size); return SQLITE_MISMATCH;
    }
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT ed25519_pubkey FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) rc = SQLITE_CORRUPT;
        else { const void* key = sqlite3_column_blob(st, 0); if (!key) rc = SQLITE_NOMEM; else { memcpy(pub, key, 32); rc = SQLITE_OK; } }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: group key unavailable group=%s rc=%d finalize=%d", channel ? channel : "-", rc, final);
        return rc;
    }
    int length = ga_message(r, bytes);
    int verified = sc_ed25519_verify(pub, bytes, length, r->signature);
    if (verified != SC_OK) {
        if (verified == SC_ERR_AUTH_FAILED) db_changes_warn(db, NULL, channel, "Group avatar", "invalid signature", pub, r->signature, NULL);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: invalid group signature group=%s version=%llu",
            channel ? channel : "-", (unsigned long long)r->version); return SQLITE_AUTH;
    }
    return 0;
}
static int ga_verify(struct UTUN_INSTANCE* inst, uint64_t group, const struct group_avatar_record* r) {
    return ga_verify_read(inst && CC(inst) ? CC(inst)->db : NULL, inst ? topo_groups_channel(inst->topo_groups, group) : NULL, r) ? -1 : 0;
}

static int ga_record_read(sqlite3* db, const char* channel, struct group_avatar_record* r) {
    if (!db || !chat_channel_valid(channel) || !r) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: invalid read group=%s", channel ? channel : "-"); return SQLITE_MISUSE;
    }
    struct group_avatar_record result = {0}; sqlite3_stmt* st = NULL;
    const char* sql = "SELECT avatar_version,avatar_data,avatar_hash,avatar_signature FROM channels WHERE channel_id=?";
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) goto done;
    stage = "bind"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC); if (rc != SQLITE_OK) goto done;
    stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto done; }
    result.version = (uint64_t)sqlite3_column_int64(st, 0); int size = sqlite3_column_bytes(st, 1);
    stage = "record fields"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || result.version > INT64_MAX || size > GROUP_AVATAR_MAX) goto done;
    if (!result.version) {
        if (size || sqlite3_column_bytes(st, 2) || sqlite3_column_bytes(st, 3)) goto done;
    } else {
        if (sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_type(st, 2) != SQLITE_BLOB ||
            sqlite3_column_type(st, 3) != SQLITE_BLOB || sqlite3_column_bytes(st, 2) != 32 || sqlite3_column_bytes(st, 3) != 64) goto done;
        const void *data = sqlite3_column_blob(st, 1), *hash = sqlite3_column_blob(st, 2), *signature = sqlite3_column_blob(st, 3);
        if ((size && !data) || !hash || !signature) { rc = SQLITE_NOMEM; goto done; }
        result.size = (uint16_t)size; if (size) memcpy(result.data, data, (size_t)size);
        memcpy(result.hash, hash, 32); memcpy(result.signature, signature, 64);
    }
    rc = SQLITE_OK;
done:
    {
        int final = sqlite3_finalize(st);
        if (rc != SQLITE_OK || final != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: read group=%s stage=%s rc=%d finalize=%d: %s",
                channel ? channel : "-", stage, rc, final, sqlite3_errmsg(db)); return rc ? rc : final;
        }
    }
    if (result.version) { int code = ga_verify_read(db, channel, &result); if (code) return code; }
    *r = result; return 0;
}
int group_avatar_get(struct UTUN_INSTANCE* inst, uint64_t group, struct group_avatar_record* r) {
    if (inst && db_async_can_write(inst->db_worker)) {
        struct group_avatar* ctx = ga_context(inst);
        struct TOPO_GROUP* topology = inst->topo_groups ? topo_groups_find(inst->topo_groups, group) : NULL;
        if (!r || !ctx || ctx->closing || !topology || topology->stopping || !inst->group_started) return -1;
        struct ga_cache* cached = ga_cached(ctx, group, topology->generation, 0);
        if (cached && cached->ready) { *r = cached->value; return 0; }
        if (!cached || !cached->loading) group_avatar_submit(inst, group, NULL, NULL);
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: snapshot loading group=%llu", (unsigned long long)group); return -1;
    }
    return ga_record_read(inst && CC(inst) ? CC(inst)->db : NULL, inst ? topo_groups_channel(inst->topo_groups, group) : NULL, r) ? -1 : 0;
}

static int ga_compare(uint64_t a, const uint8_t ah[32], uint64_t b, const uint8_t bh[32]) {
    return a != b ? (a > b ? 1 : -1) : memcmp(ah, bh, 32);
}

static void ga_changed(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct group_avatar* ctx = ga_context(inst);
    for (struct ga_peer* p = ctx ? ctx->peers : NULL; p; p = p->next) if (p->group == group) {
        p->state_pending = 1; ga_schedule(p);
    }
    const char* ch = topo_groups_channel(inst->topo_groups, group); uint8_t event[65]; event[0] = (uint8_t)strlen(ch);
    memcpy(event + 1, ch, event[0]); chat_event_post(inst, CHAT_EVT_CHANNEL_UPDATED, event, event[0] + 1);
}

/* Блокировка охватывает проверку допуска/версии и запись; callback здесь не вызывается. */
static int ga_begin(struct UTUN_INSTANCE* inst, uint64_t group) {
    if (!inst || !CC(inst) || !CC(inst)->initialized || !CC(inst)->db || db_async_can_write(inst->db_worker) ||
        !group || group > INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: inactive core/invalid group=%llu", (unsigned long long)group); return -1;
    }
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: external transaction rejected group=%llu", (unsigned long long)group); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* group avatar */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: begin group=%llu rc=%d: %s",
        (unsigned long long)group, rc, sqlite3_errmsg(db));
    return rc == SQLITE_OK ? 0 : -1;
}

static int ga_finish(struct UTUN_INSTANCE* inst, uint64_t group, int result) {
    sqlite3* db = CC(inst)->db; int rc;
    if (result > 0) {
        rc = sqlite3_exec(db, "COMMIT /* group avatar */", NULL, NULL, NULL);
        if (rc == SQLITE_OK) return result;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: commit group=%llu rc=%d: %s",
            (unsigned long long)group, rc, sqlite3_errmsg(db)); result = -1;
    }
    if (!sqlite3_get_autocommit(db)) {
        rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: rollback group=%llu rc=%d: %s",
                (unsigned long long)group, rc, sqlite3_errmsg(db)); result = -1;
        }
        if (!sqlite3_get_autocommit(db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: transaction remains open group=%llu; requesting owner stop/DB close",
                (unsigned long long)group); utun_instance_stop(inst); result = -1;
        }
    }
    return result;
}

static int ga_record_write(sqlite3* db, const char* channel, const struct group_avatar_record* r) {
    sqlite3_stmt* st = NULL;
    const char* sql = "UPDATE channels SET avatar_version=?,avatar_data=?,avatar_hash=?,avatar_signature=? WHERE channel_id=?";
    const char* stage = "prepare"; int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind version"; rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)r->version); }
    if (rc == SQLITE_OK) { stage = "bind data"; rc = sqlite3_bind_blob(st, 2, r->data, r->size, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind hash"; rc = sqlite3_bind_blob(st, 3, r->hash, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind signature"; rc = sqlite3_bind_blob(st, 4, r->signature, 64, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind channel"; rc = sqlite3_bind_text(st, 5, channel, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    if (rc == SQLITE_DONE && sqlite3_changes(db) != 1) { stage = "changes != 1"; rc = SQLITE_CONSTRAINT; }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: write group=%s stage=%s rc=%d finalize=%d: %s",
            channel ? channel : "-", stage, rc, final, sqlite3_errmsg(db)); return rc == SQLITE_DONE ? final : rc;
    }
    return SQLITE_OK;
}
static int ga_write(struct UTUN_INSTANCE* inst, uint64_t group, const struct group_avatar_record* r) {
    return ga_record_write(CC(inst)->db, topo_groups_channel(inst->topo_groups, group), r) ? -1 : 1;
}

enum ga_job_mode { GA_READ_RECORD, GA_SET_RECORD, GA_APPLY_RECORD };
struct ga_job {
    uint8_t peer[32];
    uint64_t group, now; enum ga_job_mode mode;
    char channel[65];
    uint8_t self_ed[32]; /* Frozen SQL входы. */
    struct group_avatar_record input;
    struct group_avatar_result result;
    struct UTUN_INSTANCE* inst; struct group_avatar* owner; /* Только guard/done. */
    uint64_t generation, registration;
    group_avatar_done_fn done; void* arg;
};

static int ga_permission_read(sqlite3* db, const char* channel, const uint8_t self_ed[32], int* can_edit, uint8_t key[32]) {
    *can_edit = 0;
    int code = SQLITE_OK;
    enum ms_author_status status = member_sync_verify_author(db, channel, self_ed, &code);
    if (status == MS_AUTHOR_ERROR) return code ? code : SQLITE_ERROR;
    if (status != MS_AUTHOR_ACTIVE) return SQLITE_OK;
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT ed25519_privkey FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        int type = sqlite3_column_type(st, 0);
        if (type == SQLITE_NULL) rc = SQLITE_OK;
        else if (type != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) rc = SQLITE_CORRUPT;
        else { const void* bytes = sqlite3_column_blob(st, 0); if (!bytes) rc = SQLITE_NOMEM;
            else { memcpy(key, bytes, 32); *can_edit = 1; rc = SQLITE_OK; } }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: edit permission read group=%s rc=%d finalize=%d",
        channel ? channel : "-", rc, final);
    return rc;
}
static int ga_job_current(void* data) {
    struct ga_job* job = data; struct UTUN_INSTANCE* inst = job->inst;
    if (inst->destroying || !inst->group_started || !chat_core_is_initialized(inst) || inst->group_generation != job->generation ||
        ga_context(inst) != job->owner) return 0;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find(inst->topo_groups, job->group) : NULL;
    return group && !group->stopping && group->group_type == TOPO_GROUP_TYPE_CHAT && group->generation == job->registration;
}
static int ga_job_work(sqlite3* db, void* data) {
    struct ga_job* job = data; uint8_t key[32] = {0}; struct group_avatar_record* current = &job->result.record;
    int code = ga_record_read(db, job->channel, current); if (code) goto done;
    code = ga_permission_read(db, job->channel, job->self_ed, &job->result.can_edit, key); if (code) goto done;
    if (job->mode == GA_READ_RECORD) goto done;
    if (job->mode == GA_SET_RECORD) {
        if (!job->result.can_edit) { code = SQLITE_AUTH; goto done; }
        uint8_t hash[32]; SHA256(job->input.data, job->input.size, hash);
        if (current->version && current->size == job->input.size && !memcmp(current->hash, hash, 32)) goto done;
        if (current->version == INT64_MAX || !job->now || job->now > INT64_MAX) { code = SQLITE_RANGE; goto done; }
        job->input.version = job->now > current->version ? job->now : current->version + 1; memcpy(job->input.hash, hash, 32);
        uint8_t bytes[GROUP_AVATAR_MAX + 64]; int size = ga_message(&job->input, bytes);
        if (sc_ed25519_sign(key, bytes, (size_t)size, job->input.signature) != SC_OK) { code = SQLITE_ERROR; goto done; }
    }
    struct db_changes_origin source = db_changes_source(db, job->peer, 0, job->mode == GA_APPLY_RECORD ? "group avatar update" : "local avatar");
    code = ga_verify_read(db, job->channel, &job->input);
    if (!code && ga_compare(job->input.version, job->input.hash, current->version, current->hash) > 0) {
        code = ga_record_write(db, job->channel, &job->input);
        if (!code) { *current = job->input; job->result.changed = 1; }
    }
    db_changes_restore(db, source);
done:
    OPENSSL_cleanse(key, sizeof(key));
    if (code) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: worker operation group=%llu mode=%d rc=%d: %s",
        (unsigned long long)job->group, job->mode, code, sqlite3_errstr(code));
    return code;
}
static void ga_job_done(void* data, const struct db_async_result* result) {
    struct ga_job* job = data; struct UTUN_INSTANCE* inst = job->inst; int current = ga_job_current(job);
    job->result.status = result->status; job->result.sqlite_code = result->sqlite_code;
    job->result.committed = !result->status && job->mode != GA_READ_RECORD;
    if (!result->status && !current) { job->result.status = DB_ASYNC_CANCELLED; job->result.sqlite_code = SQLITE_ABORT; }
    struct group_avatar* ctx = ga_context(inst);
    if (ctx == job->owner && inst->group_generation == job->generation) {
        struct ga_cache* cached = ctx->cache;
        while (cached && (cached->group != job->group || cached->registration != job->registration)) cached = cached->next;
        if (cached) {
            if (cached->loading) cached->loading--;
            if (!result->status && current) {
                cached->value = job->result.record; cached->can_edit = job->result.can_edit; cached->ready = 1;
                for (struct ga_peer* peer = ctx->peers; peer; peer = peer->next) if (peer->group == job->group) ga_schedule(peer);
            }
        }
    }
    if (job->result.status) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: completion group=%llu mode=%d status=%d rc=%d committed=%d: %s",
        (unsigned long long)job->group, job->mode, job->result.status, job->result.sqlite_code, job->result.committed, result->message);
    if (!job->result.status && job->mode != GA_READ_RECORD && job->result.changed) ga_changed(inst, job->group);
    if (job->done) job->done(job->arg, &job->result);
}
static void ga_job_free(void* data) { u_free(data); }
static void ga_refused(uint64_t group, group_avatar_done_fn done, void* arg, int status, int code) {
    struct group_avatar_result result = {.status=status, .sqlite_code=code};
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: request refused group=%llu status=%d rc=%d", (unsigned long long)group, status, code);
    if (done) done(arg, &result);
}
static void ga_submit(struct UTUN_INSTANCE* inst, uint64_t group, enum ga_job_mode mode,
    const struct group_avatar_record* input, const uint8_t* data, size_t size, const uint8_t* peer, group_avatar_done_fn done, void* arg) {
    struct group_avatar* ctx = ga_context(inst);
    struct TOPO_GROUP* topology = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group) : NULL;
    if (!inst || !ctx || ctx->closing || !inst->group_started || !chat_core_is_initialized(inst) || !group || group > INT64_MAX ||
        !topology || topology->stopping || topology->group_type != TOPO_GROUP_TYPE_CHAT ||
        (mode == GA_SET_RECORD && (size > GROUP_AVATAR_MAX || (size && !data))) || (mode == GA_APPLY_RECORD && !input)) {
        ga_refused(group, done, arg, DB_ASYNC_INVALID, SQLITE_MISUSE); return;
    }
    struct ga_cache* cached = ga_cached(ctx, group, topology->generation, 1);
    struct ga_job* job = cached ? u_calloc(1, sizeof(*job)) : NULL;
    if (!job) { ga_refused(group, done, arg, DB_ASYNC_NOMEM, SQLITE_NOMEM); return; }
    job->group = group; strcpy(job->channel, topology->channel_id); memcpy(job->self_ed, inst->my_ed25519_pubkey, 32); job->mode = mode; job->inst = inst; job->owner = ctx;
    job->generation = inst->group_generation; job->registration = topology->generation; job->done = done; job->arg = arg;
    int64_t now = ntp_time_get_us(inst); job->now = now > 0 ? (uint64_t)now / 1000 : 0;
    if (input) job->input = *input;
    if (peer) memcpy(job->peer, peer, 32);
    if (mode == GA_SET_RECORD) { job->input.size = (uint16_t)size; if (size) memcpy(job->input.data, data, size); }
    cached->loading++;
    if (inst->db_worker && (mode == GA_READ_RECORD || db_async_can_write(inst->db_worker))) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="group-avatar", .mode=mode == GA_READ_RECORD ? DB_ASYNC_READ : DB_ASYNC_WRITE,
            .guard=ga_job_current, .work=ga_job_work, .done=ga_job_done, .cleanup=ga_job_free, .data=job}); return;
    }
    sqlite3* db = CC(inst)->db; struct db_async_result result = {.status=DB_ASYNC_SQL, .sqlite_code=SQLITE_MISUSE};
    int owns = 0, code = db && sqlite3_get_autocommit(db) ? SQLITE_OK : SQLITE_MISUSE;
    const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    if (!inst->db_worker && (!file || *file)) code = SQLITE_MISUSE;
    if (!code) { code = sqlite3_exec(db, mode == GA_READ_RECORD ? "BEGIN" : "BEGIN IMMEDIATE", NULL, NULL, NULL); owns = !code; }
    if (!code) code = ga_job_work(db, job);
    if (!code) code = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (code && owns && !sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: legacy job rollback failed"); if (!sqlite3_get_autocommit(db)) utun_instance_stop(inst);
    }
    result.status = code ? DB_ASYNC_SQL : DB_ASYNC_OK; result.sqlite_code = code;
    snprintf(result.message, sizeof(result.message), "%s", sqlite3_errstr(code)); ga_job_done(job, &result); ga_job_free(job);
}
void group_avatar_submit(struct UTUN_INSTANCE* inst, uint64_t group, group_avatar_done_fn done, void* arg) {
    ga_submit(inst, group, GA_READ_RECORD, NULL, NULL, 0, NULL, done, arg);
}
void group_avatar_set_submit(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t* data, size_t size,
    group_avatar_done_fn done, void* arg) { ga_submit(inst, group, GA_SET_RECORD, NULL, data, size, NULL, done, arg); }
void group_avatar_apply_submit(struct UTUN_INSTANCE* inst, uint64_t group, const struct group_avatar_record* record,
    group_avatar_done_fn done, void* arg) { ga_submit(inst, group, GA_APPLY_RECORD, record, NULL, 0, NULL, done, arg); }

/* Приём завершает всю работу с ga_peer до публикации события, способного удалить группу. */
static int ga_apply_transaction(struct UTUN_INSTANCE* inst, uint64_t group, const struct group_avatar_record* r) {
    if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: missing candidate record"); return -1; }
    if (ga_begin(inst, group) < 0) return -1;
    int result = -1; struct group_avatar_record current;
    if (ga_verify(inst, group, r) < 0 || group_avatar_get(inst, group, &current) < 0) goto done;
    int newer = ga_compare(r->version, r->hash, current.version, current.hash);
    if (r->version == current.version && memcmp(r->hash, current.hash, 32))
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: concurrent versions group=%llu version=%llu accept=%d",
            (unsigned long long)group, (unsigned long long)r->version, newer > 0);
    result = newer > 0 ? ga_write(inst, group, r) : 0;
done:
    result = ga_finish(inst, group, result);
    if (result > 0) DEBUG_INFO(DEBUG_CATEGORY_CHAT, "group avatar: saved group=%llu version=%llu bytes=%u committed=1",
        (unsigned long long)group, (unsigned long long)r->version, r->size);
    return result;
}

int group_avatar_apply(struct UTUN_INSTANCE* inst, uint64_t group, const struct group_avatar_record* r) {
    int result = ga_apply_transaction(inst, group, r);
    if (result > 0) ga_changed(inst, group);
    return result;
}

int group_avatar_set(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t* data, size_t size) {
    if (size > GROUP_AVATAR_MAX || (size && !data)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: invalid publication group=%llu bytes=%zu", (unsigned long long)group, size); return -1;
    }
    if (ga_begin(inst, group) < 0) return -1;
    int result = -1; uint8_t key[32] = {0}, bytes[GROUP_AVATAR_MAX + 64]; const char* ch = topo_groups_channel(inst->topo_groups, group);
    struct group_avatar_record r;
    if (!member_files_member(inst, group, inst->my_ed25519_pubkey, 0)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: publication requires active membership group=%llu", (unsigned long long)group); goto done;
    }
    if (topo_node_sqlite_channel_get_priv(CC(inst)->db, ch, key) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: publication requires group key group=%llu", (unsigned long long)group); goto done;
    }
    if (group_avatar_get(inst, group, &r) < 0) goto done;
    uint8_t hash[32]; SHA256(size ? data : r.data, size, hash);
    if (r.version && r.size == size && !memcmp(r.hash, hash, 32)) { result = 0; goto done; }
    if (r.version == INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: version exhausted group=%llu", (unsigned long long)group); goto done;
    }
    int64_t us = ntp_time_get_us(inst); uint64_t now = us > 0 ? (uint64_t)us / 1000 : 0;
    r.version = now > r.version ? now : r.version + 1; r.size = (uint16_t)size;
    memcpy(r.hash, hash, 32); if (size) memcpy(r.data, data, size);
    int n = ga_message(&r, bytes);
    if (sc_ed25519_sign(key, bytes, (size_t)n, r.signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: signing failed group=%llu", (unsigned long long)group); goto done;
    }
    if (ga_verify(inst, group, &r) < 0) goto done;
    result = ga_write(inst, group, &r);
done:
    OPENSSL_cleanse(key, sizeof(key)); result = ga_finish(inst, group, result);
    if (result > 0) {
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "group avatar: saved group=%llu version=%llu bytes=%u committed=1",
            (unsigned long long)group, (unsigned long long)r.version, r.size); ga_changed(inst, group);
    }
    return result < 0 ? -1 : 0;
}

static void ga_gui_done(void* arg, const struct group_avatar_result* outcome) {
    struct group_avatar_gui_request* q = arg;
    uint8_t result[CHAT_AVATAR_RESULT_SIZE]; mf_put64(result, q->id); memcpy(result + 8, q->group, 32);
    result[CHAT_AVATAR_RESULT_STATUS] = outcome->status ? 1 : 0;
    chat_event_post(q->inst, CHAT_EVT_GROUP_AVATAR_RESULT, result, sizeof(result)); u_free(q);
}
void group_avatar_gui_set(void* arg) {
    struct group_avatar_gui_request* q = arg;
    if (!q || !q->inst) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: invalid GUI request"); u_free(q); return; }
    struct TOPO_GROUP* group = topo_groups_find_wire(q->inst->topo_groups, q->group);
    group_avatar_set_submit(q->inst, group && group->group_type == TOPO_GROUP_TYPE_CHAT ? group->group_id : 0,
        q->data, q->size, ga_gui_done, q);
}

static const struct TOPO_GROUP_CONN_ITEM* ga_session(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32]) {
    struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, group);
    for (struct ll_entry* e = g && g->senders_list ? g->senders_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (item && item->conn && item->ready_notified && !item->transport_down && !memcmp(item->conn->peer_ed25519_pubkey, ed, 32))
            return item;
    }
    return NULL;
}

static struct ga_peer* ga_find(struct group_avatar* ctx, uint64_t group, const uint8_t ed[32], int create) {
    for (struct ga_peer* p = ctx->peers; p; p = p->next) if (p->group == group && !memcmp(p->ed, ed, 32)) return p;
    if (!create) return NULL;
    struct ga_peer* p = u_calloc(1, sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: peer allocation failed"); return NULL; }
    p->owner = ctx; p->group = group; memcpy(p->ed, ed, 32); p->state_pending = 1;
    const struct TOPO_GROUP_CONN_ITEM* item = ga_session(ctx->inst, group, ed);
    if (item) p->epoch = item->local_epoch;
    p->next = ctx->peers; ctx->peers = p; return p;
}

static int ga_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32]) {
    return topo_group_chat_key_allowed(inst, group, ed) && ga_session(inst, group, ed);
}

static void ga_release(struct ga_peer* p, int sending) {
    struct group_avatar_record** record = sending ? &p->tx : &p->rx;
    if (!*record) return;
    u_free(*record); *record = NULL;
    if (sending) p->owner->tx_count--; else p->owner->rx_count--;
    if (!p->owner->closing) for (struct ga_peer* other = p->owner->peers; other; other = other->next) {
        ga_schedule(other);
        if (!sending) etcp_router_conn_resume_receive(etcp_router_conn_lookup(other->owner->inst, other->group, other->ed,
                                                                            ETCP_RT_ID_GROUP_AVATAR));
    }
}

static void ga_free(struct ga_peer* p) {
    if (p->pump) uasync_call_soon_cancel(p->owner->inst->ua, p->pump);
    etcp_router_cancel_send_ready(&p->waiter);
    ga_release(p, 1); ga_release(p, 0); u_free(p);
}

static void ga_ready(struct ll_queue* queue, void* arg) { (void)queue; ga_schedule(arg); }
static void ga_schedule(struct ga_peer* p) {
    if (p->owner->closing || p->pump || p->waiter.internal || p->waiter.call_soon_id) return;
    if (!p->state_pending && !p->get_pending && !p->reply_pending && !p->tx) return;
    p->pump = uasync_call_soon(p->owner->inst->ua, p, ga_pump);
    if (!p->pump) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: send scheduling failed");
}

static void ga_state(uint8_t* bytes, int kind, uint64_t version, uint16_t size, const uint8_t hash[32]) {
    bytes[0] = GA_WIRE_VERSION; bytes[1] = (uint8_t)kind; mf_put64(bytes + 2, version);
    bytes[10] = (uint8_t)size; bytes[11] = (uint8_t)(size >> 8); memcpy(bytes + 12, hash, 32);
}

static void ga_pump(void* arg) {
    struct ga_peer* p = arg; p->pump = NULL; struct group_avatar* ctx = p->owner;
    if (!ga_allowed(ctx->inst, p->group, p->ed)) return;
    struct ETCP_ROUTER_CONN* router = etcp_router_conn_get_key(ctx->inst, p->group, p->ed, ETCP_RT_ID_GROUP_AVATAR);
    if (!router) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: router allocation failed"); return; }
    char ed_hex[65];
    queue_set_threshold(router->send_q, 0, 0);
    for (unsigned step = 0; step < 4; step++) {
        if (!p->state_pending && !p->get_pending && !p->reply_pending && !p->tx) break;
        if (!etcp_router_conn_send_q_has_room(router)) {
            etcp_router_conn_on_send_ready(router, &p->waiter, ga_ready, p); return;
        }
        uint8_t packet[GA_DATA_HEADER + GROUP_AVATAR_BLOCK]; size_t length = GA_STATE_BYTES; int kind; uint16_t block = 0;
        struct group_avatar_record current;
        if (p->state_pending) {
            if (group_avatar_get(ctx->inst, p->group, &current) < 0) return;
            kind = GA_STATE; ga_state(packet, kind, current.version, current.size, current.hash);
        } else if (p->get_pending) { kind = GA_GET; ga_state(packet, kind, p->remote_version, 0, p->remote_hash); }
        else {
            if (!p->tx) {
                if (ctx->tx_count >= GA_STREAMS) return;
                if (group_avatar_get(ctx->inst, p->group, &current) < 0) return;
                p->tx = u_malloc(sizeof(*p->tx));
                if (!p->tx) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: send snapshot allocation failed"); return; }
                ctx->tx_count++; p->tx_offset = 0; p->tx_at = get_time_tb(); p->reply_pending = 0;
                *p->tx = current;
                if (!p->tx->version) { ga_release(p, 1); p->state_pending = 1; continue; }
            }
            kind = GA_DATA; block = p->tx->size - p->tx_offset;
            if (block > GROUP_AVATAR_BLOCK) block = GROUP_AVATAR_BLOCK;
            ga_state(packet, kind, p->tx->version, p->tx->size, p->tx->hash);
            memcpy(packet + 44, p->tx->signature, 64);
            packet[108] = (uint8_t)p->tx_offset; packet[109] = (uint8_t)(p->tx_offset >> 8);
            packet[110] = (uint8_t)block; packet[111] = (uint8_t)(block >> 8);
            memcpy(packet + GA_DATA_HEADER, p->tx->data + p->tx_offset, block); length = GA_DATA_HEADER + block;
        }
        if (etcp_router_conn_send(router, packet, length, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: send failed group=%llu peer=%s kind=%d",
                (unsigned long long)p->group, sc_public_key_hex(p->ed, ed_hex), kind); return;
        }
        if (kind == GA_STATE) { p->state_pending = 0; p->advertised_at = get_time_tb(); }
        else if (kind == GA_GET) { p->get_pending = 0; p->requested_at = get_time_tb(); }
        else {
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: block sent group=%llu peer=%s version=%llu offset=%u bytes=%u total=%u",
                (unsigned long long)p->group, sc_public_key_hex(p->ed, ed_hex), (unsigned long long)p->tx->version,
                p->tx_offset, block, p->tx->size);
            p->tx_offset += block; p->tx_at = get_time_tb();
            if (p->tx_offset == p->tx->size) ga_release(p, 1);
        }
    }
    ga_schedule(p);
}

static int ga_receive_ready(const struct ETCP_ROUTER_CONN* route, const uint8_t* data, size_t size) {
    struct group_avatar* ctx = ga_context(route->inst);
    if (!ctx || size < GA_DATA_HEADER || data[0] != GA_WIRE_VERSION || data[1] != GA_DATA ||
        !ga_allowed(route->inst, route->group_id, route->remote_ed)) return 1;
    struct ga_peer* p = ga_find(ctx, route->group_id, route->remote_ed, 0);
    return (p && p->rx) || ctx->rx_count < GA_STREAMS;
}

static void ga_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct group_avatar* ctx = conn ? ga_context(conn->instance) : NULL;
    if (!ctx || ctx->closing || !entry || !entry->dgram || entry->len < ROUTER_SVC_PAYLOAD_OFF) goto out;
    uint64_t group; memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* ed = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    char ed_hex[65];
    struct ga_peer* p = ga_find(ctx, group, ed, 0);
    if (entry->len == ROUTER_SVC_PAYLOAD_OFF) {
        if (!topo_group_chat_key_allowed(ctx->inst, group, ed)) { group_avatar_revalidate_members(ctx->inst, group); goto out; }
        if (p) { ga_release(p, 1); ga_release(p, 0); p->requested_at = 0; p->state_pending = 1; ga_schedule(p); }
        goto out;
    }
    if ((entry->dgram[ROUTER_SVC_FLAGS_OFF] & (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) !=
        (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) || !ga_allowed(ctx->inst, group, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: unauthorized packet group=%llu peer=%s",
            (unsigned long long)group, sc_public_key_hex(ed, ed_hex)); goto out;
    }
    const uint8_t* bytes = entry->dgram + ROUTER_SVC_PAYLOAD_OFF; size_t length = entry->len - ROUTER_SVC_PAYLOAD_OFF;
    if (length < GA_STATE_BYTES || bytes[0] != GA_WIRE_VERSION || bytes[1] < GA_STATE || bytes[1] > GA_DATA) goto malformed;
    uint64_t version = mf_get64(bytes + 2); uint16_t size = bytes[10] | ((uint16_t)bytes[11] << 8);
    if (version > INT64_MAX || size > GROUP_AVATAR_MAX) goto malformed;
    if (!p) p = ga_find(ctx, group, ed, 1);
    if (!p) goto out;
    if (bytes[1] != GA_DATA) {
        if (length != GA_STATE_BYTES) goto malformed;
        struct group_avatar_record local; if (group_avatar_get(ctx->inst, group, &local) < 0) goto out;
        if (bytes[1] == GA_GET) {
            if (!p->tx) p->reply_pending = 1;
        } else {
            p->remote_version = version; memcpy(p->remote_hash, bytes + 12, 32);
            int newer = ga_compare(version, p->remote_hash, local.version, local.hash);
            if (newer > 0 && (!p->requested_at || get_time_tb() - p->requested_at >= GA_RETRY_TB)) p->get_pending = 1;
            if (newer < 0) p->state_pending = 1;
            if (!newer) p->requested_at = 0;
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: state group=%llu peer=%s remote=%llu local=%llu request=%d",
                (unsigned long long)group, sc_public_key_hex(ed, ed_hex), (unsigned long long)version,
                (unsigned long long)local.version, p->get_pending);
        }
        ga_schedule(p); goto out;
    }
    if (length < GA_DATA_HEADER || !version) goto malformed;
    uint16_t offset = bytes[108] | ((uint16_t)bytes[109] << 8), block = bytes[110] | ((uint16_t)bytes[111] << 8);
    if (offset > size || block > GROUP_AVATAR_BLOCK || block > size - offset || length != GA_DATA_HEADER + block ||
        (size && !block) || (offset + block < size && block != GROUP_AVATAR_BLOCK)) goto malformed;
    if (!offset) {
        ga_release(p, 0);
        if (ctx->rx_count >= GA_STREAMS) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: receive limit reached"); goto out; }
        p->rx = u_calloc(1, sizeof(*p->rx));
        if (!p->rx) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: receive allocation failed"); goto out; }
        ctx->rx_count++; p->rx_offset = 0; p->rx->version = version; p->rx->size = size;
        memcpy(p->rx->hash, bytes + 12, 32); memcpy(p->rx->signature, bytes + 44, 64);
    }
    if (!p->rx || p->rx_offset != offset || p->rx->version != version || p->rx->size != size ||
        memcmp(p->rx->hash, bytes + 12, 32) || memcmp(p->rx->signature, bytes + 44, 64)) goto malformed;
    memcpy(p->rx->data + offset, bytes + GA_DATA_HEADER, block); p->rx_offset += block; p->rx_at = get_time_tb();
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: block received group=%llu peer=%s version=%llu offset=%u bytes=%u total=%u",
        (unsigned long long)group, sc_public_key_hex(ed, ed_hex), (unsigned long long)version, offset, block, size);
    if (p->rx_offset == size) {
        if (db_async_can_write(ctx->inst->db_worker)) {
            ga_submit(ctx->inst, group, GA_APPLY_RECORD, p->rx, NULL, 0, ed, NULL, NULL); ga_release(p, 0);
            p->requested_at = 0; p->state_pending = 1; ga_schedule(p); goto out;
        }
        sqlite3* db = CC(ctx->inst)->db;
        struct db_changes_origin source = db_changes_source(db, ed, 0, "group avatar update");
        int applied = ga_apply_transaction(ctx->inst, group, p->rx);
        db_changes_restore(db, source);
        ga_release(p, 0);
        p->requested_at = applied < 0 ? get_time_tb() : 0;
        p->state_pending = 1; ga_schedule(p);
        if (applied > 0) ga_changed(ctx->inst, group);
    }
    goto out;
malformed:
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: malformed/mixed blocks group=%llu peer=%s bytes=%zu",
        (unsigned long long)group, sc_public_key_hex(ed, ed_hex), length);
    if (p) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: receive state rx=%d offset=%u version=%llu requested=%llu",
        p->rx != NULL, p->rx_offset, (unsigned long long)(p->rx ? p->rx->version : 0), (unsigned long long)p->requested_at);
    if (p) { ga_release(p, 0); p->requested_at = get_time_tb(); }
out:
    if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
}

static void ga_tick(void* arg) {
    struct group_avatar* ctx = arg; ctx->timer = NULL; uint64_t now = get_time_tb();
    for (struct ga_peer* p = ctx->peers; p; p = p->next) p->seen = 0;
    for (struct ll_entry* ge = ctx->inst->topo_groups->group_list->head; ge; ge = ge->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)ge;
        if (group->group_type != TOPO_GROUP_TYPE_CHAT || !group->senders_list) continue;
        for (struct ll_entry* e = group->senders_list->head; e; e = e->next) {
            struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data;
            if (!item || !item->conn || !item->ready_notified || item->transport_down) continue;
            const uint8_t* ed = item->conn->peer_ed25519_pubkey;
            if (!topo_group_chat_key_allowed(ctx->inst, group->group_id, ed)) continue;
            struct ga_peer* p = ga_find(ctx, group->group_id, ed, 1); if (!p) continue;
            char ed_hex[65];
            p->seen = 1;
            if (p->epoch != item->local_epoch) {
                DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: session epoch reset group=%llu peer=%s old=%llu new=%llu tx=%d rx=%d",
                    (unsigned long long)p->group, sc_public_key_hex(p->ed, ed_hex), (unsigned long long)p->epoch,
                    (unsigned long long)item->local_epoch, p->tx != NULL, p->rx != NULL);
                ga_release(p, 1); ga_release(p, 0); p->epoch = item->local_epoch; p->requested_at = 0; p->state_pending = 1;
            }
            if (now - p->advertised_at >= GA_EXCHANGE_TB) p->state_pending = 1;
            if (p->requested_at && now - p->requested_at >= GA_RETRY_TB) { p->get_pending = 1; p->requested_at = 0; }
            if ((p->rx && now - p->rx_at >= GA_RETRY_TB) || (p->tx && now - p->tx_at >= GA_RETRY_TB)) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: incomplete transfer expired group=%llu peer=%s",
                    (unsigned long long)p->group, sc_public_key_hex(p->ed, ed_hex)); ga_release(p, 1); ga_release(p, 0); p->state_pending = 1;
            }
            ga_schedule(p);
        }
    }
    struct ga_peer** link = &ctx->peers;
    while (*link) { struct ga_peer* p = *link; if (p->seen) link = &p->next; else { *link = p->next; ga_free(p); } }
    ctx->timer = uasync_set_timeout(ctx->inst->ua, 10000, ctx, ga_tick, "group_avatar");
    if (!ctx->timer) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: timer allocation failed");
}

static int ga_schema(sqlite3* db) {
    return db_schema_table(db, "channels", NULL);
}

int group_avatar_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst) || !CC(inst)->db) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: core unavailable"); return -1; }
    if (ga_context(inst)) return 0;
    if (ga_schema(CC(inst)->db) < 0) return -1;
    return group_avatar_activate(inst);
}

int group_avatar_activate(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: runtime core unavailable"); return -1; }
    if (ga_context(inst)) return 0;
    struct group_avatar* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: context allocation failed"); return -1; }
    ctx->inst = inst; CC(inst)->group_avatar = ctx;
    if (etcp_router_bind(inst, ETCP_RT_ID_GROUP_AVATAR, ga_receive)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: router registration failed"); CC(inst)->group_avatar = NULL; u_free(ctx); return -1;
    }
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_GROUP_AVATAR, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_GROUP_AVATAR, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: router access policy initialization failed");
        group_avatar_destroy(inst); return -1;
    }
    etcp_router_set_receive_ready(inst, ETCP_RT_ID_GROUP_AVATAR, ga_receive_ready);
    ctx->timer = uasync_set_timeout(inst->ua, 10000, ctx, ga_tick, "group_avatar");
    if (!ctx->timer) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: initial timer failed"); group_avatar_destroy(inst); return -1; }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "group avatar: initialized max_bytes=%d block_bytes=%d", GROUP_AVATAR_MAX, GROUP_AVATAR_BLOCK); return 0;
}

int group_avatar_schema_init(sqlite3* db) { return ga_schema(db); }

void group_avatar_cancel_group(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct group_avatar* ctx = ga_context(inst); if (!ctx) return;
    struct ga_peer** link = &ctx->peers;
    while (*link) { struct ga_peer* p = *link; if (p->group != group) link = &p->next; else { *link = p->next; ga_free(p); } }
    struct ga_cache** cached = &ctx->cache;
    while (*cached) { struct ga_cache* item = *cached;
        if (item->group != group) cached = &item->next; else { *cached = item->next; u_free(item); } }
}

void group_avatar_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct group_avatar* ctx = ga_context(inst);
    if (!ctx || ctx->closing) return;
    struct ga_peer** link = &ctx->peers;
    while (*link) {
        struct ga_peer* p = *link;
        if (p->group != group || topo_group_chat_key_allowed(inst, group, p->ed)) { link = &p->next; continue; }
        char ed_hex[65];
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "group avatar: revoke group=%llx peer=%s tx=%d rx=%d pending=%d/%d/%d",
            (unsigned long long)group, sc_public_key_hex(p->ed, ed_hex), p->tx != NULL, p->rx != NULL,
            p->state_pending, p->get_pending, p->reply_pending);
        *link = p->next; ga_free(p);
    }
}

void group_avatar_destroy(struct UTUN_INSTANCE* inst) {
    struct group_avatar* ctx = ga_context(inst); if (!ctx) return;
    ctx->closing = 1; if (ctx->timer) uasync_cancel_timeout(inst->ua, ctx->timer);
    etcp_router_set_receive_ready(inst, ETCP_RT_ID_GROUP_AVATAR, NULL);
    etcp_router_unbind(inst, ETCP_RT_ID_GROUP_AVATAR);
    while (ctx->peers) { struct ga_peer* p = ctx->peers; ctx->peers = p->next; ga_free(p); }
    while (ctx->cache) { struct ga_cache* cached = ctx->cache; ctx->cache = cached->next; u_free(cached); }
    CC(inst)->group_avatar = NULL; u_free(ctx); DEBUG_INFO(DEBUG_CATEGORY_CHAT, "group avatar: stopped");
}
