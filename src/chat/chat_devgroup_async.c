#include "chat_devgroup.h"
#include "chat_core.h"
#include "chat_event.h"
#include "group_policy.h"
#include "member_sync.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../routing_layer/topo_node.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../dm/dm_core.h"
#include "../dm/dm_media.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <limits.h>
#include <string.h>

enum cd_action { CD_RENAME, CD_LEAVE, CD_REFRESH, CD_ENSURE, CD_IMPORT, CD_RECEIPT };
struct cd_job {
    enum cd_action action; uint64_t self, group, now;
    char name[64], channel[65]; uint8_t receipt[16], device_x[32], device_ed[32], signer[32], peer_ed[32];
    struct devgroup_local_identity identity; struct devgroup_record input; uint8_t input_ed[32], input_x[32];
    struct TOPO_NODE* node; /* Heap freeze, принадлежащий worker на время work. */
    struct UTUN_INSTANCE* inst; struct TOPO_NODE* runtime; uint64_t generation, common_generation; /* Только uasync. */
    int leaving, close_dm, guard_error; struct chat_devgroup_result result;
    chat_devgroup_done_fn done; void* arg;
};
static int cd_current(struct cd_job* job) {
    return !job->inst->destroying && job->inst->group_started && chat_core_is_initialized(job->inst) &&
        chat_core_generation(job->inst) == job->generation && job->inst->group_generation == job->common_generation;
}
static int cd_guard(void* data) {
    struct cd_job* job = data; struct UTUN_INSTANCE* inst = job->inst;
    if (!cd_current(job)) return 0;
    if (job->action == CD_IMPORT || job->action == CD_ENSURE) {
        int allowed = job->action == CD_IMPORT ? chat_devgroup_import_allowed(inst, job->group, job->peer_ed, job->receipt) :
            chat_devgroup_ensure_allowed(inst, job->group, job->peer_ed, job->receipt);
        if (!allowed) return 0;
    }
    if (job->action == CD_RECEIPT) return 1;
    struct TOPO_NODE* current = topo_node_registry_lookup(inst->topo_groups, inst->node_id, inst->my_keys.public_key);
    if (!current) { job->guard_error = SQLITE_NOTFOUND; return 0; }
    job->node = topo_node_freeze(current); job->runtime = job->node ? topo_node_clone(inst->topo_groups, current) : NULL;
    if (!job->node || !job->runtime) { job->guard_error = SQLITE_NOMEM; return 0; }
    uint64_t previous = inst->local_snapshot_reserved > current->timestamp ? inst->local_snapshot_reserved : current->timestamp;
    if (previous >= INT64_MAX) { job->guard_error = SQLITE_RANGE; return 0; }
    int64_t now = ntp_time_get_us(inst); job->now = now > 0 ? (uint64_t)now : 1;
    job->node->timestamp = job->now > previous ? job->now : previous + 1; job->node->ver = (current->ver % 255) + 1;
    inst->local_snapshot_reserved = job->node->timestamp;
    return 1;
}
static int cd_receipt_read(sqlite3* db, struct cd_job* job) {
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT peer FROM devgroup_receipts WHERE device_ed=? AND id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->device_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, job->receipt, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) rc = SQLITE_CORRUPT;
        else if (memcmp(sqlite3_column_blob(st, 0), job->peer_ed, 32)) rc = SQLITE_CONSTRAINT;
        else { job->result.found = 1; rc = SQLITE_OK; }
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); return rc ? rc : final;
}
static int cd_store(sqlite3* db, struct cd_job* job) {
    sqlite3_stmt* st = NULL; uint8_t wire[DEVGROUP_WIRE_MAX]; int bytes = 0;
    if (job->identity.present) {
        bytes = devgroup_encode(&job->identity.record, wire, sizeof(wire)); if (bytes <= 0) return SQLITE_CONSTRAINT;
    }
    int rc = sqlite3_prepare_v2(db, job->identity.present ?
        "INSERT INTO devgroup_local(device_ed,ed_priv,x_priv,record) VALUES(?,?,?,?) ON CONFLICT(device_ed)"
        " DO UPDATE SET ed_priv=excluded.ed_priv,x_priv=excluded.x_priv,record=excluded.record" :
        "DELETE FROM devgroup_local WHERE device_ed=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->device_ed, 32, SQLITE_STATIC);
    if (!rc && job->identity.present) rc = sqlite3_bind_blob(st, 2, job->identity.ed_private, 32, SQLITE_STATIC);
    if (!rc && job->identity.present) rc = sqlite3_bind_blob(st, 3, job->identity.x_private, 32, SQLITE_STATIC);
    if (!rc && job->identity.present) rc = sqlite3_bind_blob(st, 4, wire, bytes, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) rc = final;
    if (!rc && job->identity.present && sqlite3_changes(db) != 1) rc = SQLITE_CONSTRAINT;
    return rc;
}
static int cd_store_receipt(sqlite3* db, struct cd_job* job) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO devgroup_receipts(device_ed,id,peer) VALUES(?,?,?)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->device_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, job->receipt, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, job->peer_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) rc = final;
    if (!rc && sqlite3_changes(db) != 1) rc = SQLITE_CONSTRAINT;
    return rc;
}
static int cd_adopt_latest(sqlite3* db, struct cd_job* job) {
    if (!job->identity.present) return SQLITE_OK;
    struct devgroup_profile current, latest; struct devgroup_record best;
    if (devgroup_profile_read(&job->identity.record, &current)) return SQLITE_CORRUPT;
    int found = devgroup_latest(db, job->identity.record.ed, &best, &latest); if (found < 0) return -found;
    if (!found || devgroup_profile_compare(&latest, &current) <= 0) return SQLITE_OK;
    if (memcmp(best.x, job->identity.record.x, 32)) return SQLITE_AUTH;
    if (devgroup_bind(&best, job->identity.ed_private, job->device_x, job->device_ed)) return SQLITE_ERROR;
    job->identity.record = best; return SQLITE_OK;
}
static int cd_peer_read(sqlite3* db, struct cd_job* job) {
    int error = SQLITE_OK;
    enum ms_author_status author = member_sync_verify_author(db, job->channel, job->peer_ed, &error);
    if (author == MS_AUTHOR_ERROR) return error ? error : SQLITE_ERROR;
    if (author != MS_AUTHOR_ACTIVE) return SQLITE_AUTH;
    author = member_sync_verify_author(db, job->channel, job->device_ed, &error);
    if (author == MS_AUTHOR_ERROR) return error ? error : SQLITE_ERROR;
    return author == MS_AUTHOR_ACTIVE ? SQLITE_OK : SQLITE_AUTH;
}
static int cd_import(sqlite3* db, struct cd_job* job) {
    int code = cd_receipt_read(db, job); if (code || job->result.found) return code;
    code = cd_peer_read(db, job); if (code) return code;
    uint8_t x[32], ed[32];
    struct devgroup_record record = job->input; struct devgroup_profile incoming;
    if (devgroup_public_keys(job->input_ed, job->input_x, ed, x) || memcmp(ed, record.ed, 32) ||
        memcmp(x, record.x, 32) || devgroup_profile_read(&record, &incoming)) return SQLITE_AUTH;
    if (job->identity.present) {
        if (memcmp(job->identity.record.ed, record.ed, 32) || memcmp(job->identity.record.x, record.x, 32)) return SQLITE_AUTH;
        struct devgroup_profile own; if (devgroup_profile_read(&job->identity.record, &own)) return SQLITE_CORRUPT;
        if (devgroup_profile_compare(&own, &incoming) > 0) record = job->identity.record;
    }
    if (devgroup_bind(&record, job->input_ed, job->device_x, job->device_ed)) return SQLITE_ERROR;
    job->identity.record = record; job->identity.present = 1;
    memcpy(job->identity.ed_private, job->input_ed, 32); memcpy(job->identity.x_private, job->input_x, 32);
    return cd_store_receipt(db, job);
}
static int cd_work(sqlite3* db, void* data) {
    struct cd_job* job = data; if (job->action == CD_RECEIPT) return cd_receipt_read(db, job);
    int found = devgroup_local_read(db, job->device_x, job->device_ed, &job->identity.record,
        job->identity.ed_private, job->identity.x_private);
    if (found < 0) return -found;
    job->identity.ready = 1; job->identity.present = found; struct devgroup_record before = job->identity.record;
    int code = SQLITE_OK;
    if (job->action == CD_ENSURE) { code = cd_peer_read(db, job); if (code) goto done; }
    if (job->action == CD_IMPORT) { code = cd_import(db, job); if (code) goto done; }
    else if (job->action == CD_LEAVE) {
        if (found && job->close_dm) {
            code = dm_group_leave_write(db, before.ed); if (!code) code = dm_media_close_write(db); if (code) goto done;
        }
        job->leaving = found; OPENSSL_cleanse(&job->identity, sizeof(job->identity)); job->identity.ready = 1;
    } else {
        code = cd_adopt_latest(db, job); if (code) goto done;
        if (job->action == CD_RENAME || (job->action == CD_ENSURE && !found)) {
            struct devgroup_profile profile = {0};
            if (found) { if (devgroup_profile_read(&job->identity.record, &profile)) { code = SQLITE_CORRUPT; goto done; } }
            else if (RAND_bytes(job->identity.ed_private, 32) != 1 || RAND_bytes(job->identity.x_private, 32) != 1 ||
                devgroup_public_keys(job->identity.ed_private, job->identity.x_private, job->identity.record.ed, job->identity.record.x)) {
                code = SQLITE_ERROR; goto done;
            }
            if (!found || strcmp(profile.name, job->name)) {
                if (profile.version == INT64_MAX) { code = SQLITE_RANGE; goto done; }
                if (devgroup_sign(&job->identity.record, job->identity.ed_private, job->name, profile.version + 1,
                    job->now / 1000000 ? job->now / 1000000 : 1, job->device_x, job->device_ed)) {
                    code = SQLITE_CONSTRAINT; goto done;
                }
                job->identity.present = 1;
            }
        }
    }
    job->result.changed = memcmp(&before, &job->identity.record, sizeof(before)) != 0 ||
        memcmp(&job->node->devgroup, &job->identity.record, sizeof(before)) != 0;
    if (!job->result.changed) goto done;
    code = cd_store(db, job); if (code) goto done;
    job->node->devgroup = job->identity.record;
    uint8_t message[TOPO_SIG_MSG_MAX_SIZE]; int bytes = topo_node_build_sig_msg(job->node, message, sizeof(message));
    if (bytes <= 0 || sc_ed25519_sign(job->signer, message, (size_t)bytes, job->node->x25519_self_sig) != SC_OK) {
        code = SQLITE_ERROR; goto done;
    }
    code = topo_node_sqlite_snapshot_write(db, job->node);
    if (!code) code = topo_node_sqlite_nodeinfo_write(db, job->device_x);
done:
    if (code) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup worker failed action=%d node=%016llx peer=%s rc=%d: %s",
        job->action, (unsigned long long)job->self, sc_public_key_hex(job->peer_ed, (char[65]){0}), code, sqlite3_errstr(code));
    return code;
}
static void cd_done(void* data, const struct db_async_result* result) {
    struct cd_job* job = data; job->result.status = result->status; job->result.sqlite_code = result->sqlite_code;
    job->result.committed = !result->status && job->action != CD_RECEIPT;
    if (result->status == DB_ASYNC_CANCELLED && job->guard_error) {
        job->result.status = job->guard_error == SQLITE_NOMEM ? DB_ASYNC_NOMEM : DB_ASYNC_SQL; job->result.sqlite_code = job->guard_error;
    }
    if (!result->status && !cd_current(job)) { job->result.status = DB_ASYNC_CANCELLED; job->result.sqlite_code = SQLITE_ABORT; }
    if (!job->result.status && job->action != CD_RECEIPT) {
        if (devgroup_local_activate(job->inst, &job->identity)) { job->result.status = DB_ASYNC_SQL; job->result.sqlite_code = SQLITE_CORRUPT; }
        else if (job->result.changed) {
            job->runtime->devgroup = job->node->devgroup; job->runtime->timestamp = job->node->timestamp; job->runtime->ver = job->node->ver;
            memcpy(job->runtime->x25519_self_sig, job->node->x25519_self_sig, sizeof(job->runtime->x25519_self_sig));
            if (topo_node_publish_self(job->inst, job->runtime)) { job->result.status = DB_ASYNC_SQL; job->result.sqlite_code = SQLITE_CORRUPT; }
            else job->runtime = NULL;
            if (job->leaving) dm_media_close_committed(job->inst);
            if (cd_current(job)) chat_event_post(job->inst, CHAT_EVT_DEVGROUP_CHANGED, NULL, 0);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup committed action=%d node=%016llx member=%d timestamp=%llu",
                job->action, (unsigned long long)job->self, job->identity.present, (unsigned long long)job->node->timestamp);
        }
        if (job->action == CD_LEAVE && cd_current(job)) chat_devgroup_cancel_pending(job->inst);
    }
    if (job->result.status) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup completion action=%d status=%d rc=%d committed=%d: %s",
        job->action, job->result.status, job->result.sqlite_code, job->result.committed, result->message);
    if (job->done) job->done(job->arg, &job->result);
}
static void cd_free(void* data) {
    struct cd_job* job = data; topo_node_frozen_free(job->node);
    if (job->runtime) topo_node_destroy(job->inst->topo_groups, job->runtime);
    OPENSSL_cleanse(job, sizeof(*job)); u_free(job);
}
static void cd_submit(struct UTUN_INSTANCE* inst, enum cd_action action, const char* name, uint64_t group, const uint8_t peer[32],
    const uint8_t receipt[16], const struct devgroup_record* record, const uint8_t ed[32], const uint8_t x[32],
    chat_devgroup_done_fn done, void* arg) {
    int code = SQLITE_MISUSE;
    if (!inst || inst->destroying || !inst->group_started || !chat_core_is_initialized(inst) || !db_async_can_write(inst->db_worker) ||
        ((action == CD_RENAME || action == CD_ENSURE) && (!name || !name[0] || strnlen(name, 64) >= 64)) ||
        ((action == CD_IMPORT || action == CD_RECEIPT || action == CD_ENSURE) && (!receipt || !peer)) ||
        ((action == CD_IMPORT || action == CD_ENSURE) && (!group || group > INT64_MAX)) ||
        (action == CD_IMPORT && (!record || !ed || !x))) goto refused;
    struct cd_job* job = u_calloc(1, sizeof(*job)); if (!job) { code = SQLITE_NOMEM; goto refused; }
    job->inst = inst; job->action = action; job->self = inst->node_id; job->group = group; if (peer) memcpy(job->peer_ed, peer, 32);
    job->generation = chat_core_generation(inst); job->common_generation = inst->group_generation; job->done = done; job->arg = arg;
    job->close_dm = inst->dm != NULL;
    memcpy(job->device_x, inst->my_keys.public_key, 32); memcpy(job->device_ed, inst->my_ed25519_pubkey, 32);
    memcpy(job->signer, inst->my_ed25519_privkey, 32);
    if (name) snprintf(job->name, sizeof(job->name), "%s", name);
    if (receipt) memcpy(job->receipt, receipt, 16);
    if (record) job->input = *record;
    if (ed) memcpy(job->input_ed, ed, 32);
    if (x) memcpy(job->input_x, x, 32);
    if (group) strcpy(job->channel, topo_groups_channel(inst->topo_groups, group));
    db_async_run(inst->db_worker, &(struct db_async_task){.name="devgroup", .mode=action == CD_RECEIPT ? DB_ASYNC_READ : DB_ASYNC_WRITE,
        .guard=cd_guard, .work=cd_work, .done=cd_done, .cleanup=cd_free, .data=job}); return;
refused:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup submission refused action=%d group=%llu peer=%s rc=%d", action,
        (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), code);
    struct chat_devgroup_result result = {.status=code == SQLITE_NOMEM ? DB_ASYNC_NOMEM : DB_ASYNC_INVALID, .sqlite_code=code};
    if (done) done(arg, &result);
}
void chat_devgroup_rename_submit(struct UTUN_INSTANCE* inst, const char* name, chat_devgroup_done_fn done, void* arg) {
    cd_submit(inst, CD_RENAME, name, 0, 0, NULL, NULL, NULL, NULL, done, arg);
}
void chat_devgroup_ensure_submit(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t receipt[16],
    const char* name, chat_devgroup_done_fn done, void* arg) {
    cd_submit(inst, CD_ENSURE, name, group, peer, receipt, NULL, NULL, NULL, done, arg);
}
void chat_devgroup_leave_submit(struct UTUN_INSTANCE* inst, chat_devgroup_done_fn done, void* arg) {
    cd_submit(inst, CD_LEAVE, NULL, 0, 0, NULL, NULL, NULL, NULL, done, arg);
}
void chat_devgroup_refresh_submit(struct UTUN_INSTANCE* inst, chat_devgroup_done_fn done, void* arg) {
    cd_submit(inst, CD_REFRESH, NULL, 0, 0, NULL, NULL, NULL, NULL, done, arg);
}
void chat_devgroup_import_submit(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t receipt[16],
    const struct devgroup_record* record, const uint8_t ed[32], const uint8_t x[32], chat_devgroup_done_fn done, void* arg) {
    cd_submit(inst, CD_IMPORT, NULL, group, peer, receipt, record, ed, x, done, arg);
}
void chat_devgroup_receipt_submit(struct UTUN_INSTANCE* inst, const uint8_t receipt[16], const uint8_t peer[32],
    chat_devgroup_done_fn done, void* arg) {
    cd_submit(inst, CD_RECEIPT, NULL, 0, peer, receipt, NULL, NULL, NULL, done, arg);
}
