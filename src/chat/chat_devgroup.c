#include "chat_devgroup.h"
#include "chat_core_priv.h"
#include "chat_member.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../dm/dm_core.h"
#include "../../lib/mem.h"
#include "../../lib/strbuf.h"
#include "../../lib/db_async.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>

static int cd_sql(sqlite3* db, const char* sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: SQL rc=%d: %s", rc, sqlite3_errmsg(db));
    return rc == SQLITE_OK ? 0 : -1;
}
static int cd_finish(sqlite3* db, sqlite3_stmt* st, int rc, int expected) {
    int final = sqlite3_finalize(st);
    if (rc == expected && final == SQLITE_OK) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: SQL rc=%d expected=%d finalize=%d: %s", rc, expected, final, sqlite3_errmsg(db));
    return -1;
}
/* Отложенную доставку снимков выполняет штатная BGP-очередь. */
static void cd_publish(struct UTUN_INSTANCE* inst) {
    for (struct ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP* g = (struct TOPO_GROUP*)e;
        if (!g->local_node || !g->senders_list) continue;
        for (struct ll_entry* s = g->senders_list->head; s; s = s->next) {
            struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)s->data;
            if (peer && peer->conn) topo_group_send_nodeinfo(g, g->local_node, peer->conn);
        }
    }
    /* Снимок self мог изменить заголовок всей devgroup, поэтому обновляем полные списки. */
    chat_event_post(inst, CHAT_EVT_DEVGROUP_CHANGED, NULL, 0);
}
int chat_devgroup_receipt(struct UTUN_INSTANCE* inst, const uint8_t receipt[16], const uint8_t peer[32]) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL; sqlite3_stmt* st = NULL;
    if (!db || db_async_can_write(inst->db_worker) || !receipt || !peer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: receipt requires legacy SQL or receipt_submit"); return -1;
    }
    int rc = sqlite3_prepare_v2(db, "SELECT peer FROM devgroup_receipts WHERE device_ed=? AND id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, receipt, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW;
    if (found) {
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) rc = SQLITE_CORRUPT;
        else rc = !memcmp(sqlite3_column_blob(st, 0), peer, 32) ? SQLITE_DONE : SQLITE_CONSTRAINT;
    }
    if (cd_finish(db, st, rc, SQLITE_DONE)) return -1;
    return found;
}
static int cd_commit(struct UTUN_INSTANCE* inst, const struct devgroup_record* r,
    const uint8_t ep[32], const uint8_t xp[32], const uint8_t receipt[16], const uint8_t peer[32]) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    struct TOPO_NODE* current = inst ? topo_node_registry_lookup(inst->topo_groups, inst->node_id, inst->my_keys.public_key) : NULL;
    if (!db || !current || !sqlite3_get_autocommit(db) ||
        devgroup_verify(r, inst->my_keys.public_key, inst->my_ed25519_pubkey)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: invalid commit context/record"); return -1;
    }
    struct TOPO_NODE next = *current; next.devgroup = *r; next.ver = (current->ver % 255) + 1;
    int leaving = current->devgroup.json[0] && !r->json[0];
    if (cd_sql(db, "BEGIN IMMEDIATE")) return -1;
    if (leaving && (dm_group_leave_prepare(inst, current->devgroup.ed) || dm_media_close_prepare(inst))) goto rollback;
    sqlite3_stmt* st = NULL; int rc;
    if (r->json[0]) {
        uint8_t wire[DEVGROUP_WIRE_MAX]; int n = devgroup_encode(r, wire, sizeof(wire));
        if (n <= 0) goto rollback;
        rc = sqlite3_prepare_v2(db, "INSERT INTO devgroup_local(device_ed,ed_priv,x_priv,record) VALUES(?,?,?,?)"
            " ON CONFLICT(device_ed) DO UPDATE SET ed_priv=excluded.ed_priv,x_priv=excluded.x_priv,record=excluded.record", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, ep, 32, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, xp, 32, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, wire, n, SQLITE_TRANSIENT);
    } else {
        rc = sqlite3_prepare_v2(db, "DELETE FROM devgroup_local WHERE device_ed=?", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    }
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int failed = cd_finish(db, st, rc, SQLITE_DONE); st = NULL;
    if (failed || topo_node_sign_self(inst, &next)) goto rollback;
    if (receipt) {
        rc = sqlite3_prepare_v2(db, "INSERT INTO devgroup_receipts(device_ed,id,peer) VALUES(?,?,?)", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, receipt, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, peer, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        failed = cd_finish(db, st, rc, SQLITE_DONE); st = NULL; if (failed) goto rollback;
    }
    if (cd_sql(db, "COMMIT")) goto rollback;
    *current = next;
    if (leaving) dm_media_close_committed(inst);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup: committed node=%016llx member=%d timestamp=%llu",
        (unsigned long long)inst->node_id, !!r->json[0], (unsigned long long)next.timestamp);
    cd_publish(inst); return 0;
rollback:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: operation rolled back");
    if (!sqlite3_get_autocommit(db) && cd_sql(db, "ROLLBACK")) utun_instance_stop(inst);
    return -1;
}
/* Подписать удостоверение данного устройства, не меняя подписанный JSON профиля. */
static int cd_rebind(struct UTUN_INSTANCE* inst, struct devgroup_record* record, const uint8_t ep[32]) {
    return devgroup_bind(record, ep, inst->my_keys.public_key, inst->my_ed25519_pubkey);
}
int chat_devgroup_refresh(struct UTUN_INSTANCE* inst) {
    if (inst && db_async_can_write(inst->db_worker)) { chat_devgroup_refresh_submit(inst, NULL, NULL); return 0; }
    struct devgroup_record local, best; struct devgroup_profile lp, bp; uint8_t ep[32], xp[32];
    int rc = devgroup_load_local(inst, &local, ep, xp);
    if (rc <= 0) goto done;
    rc = devgroup_latest(inst->topo_sqlite_db, local.ed, &best, &bp);
    if (rc <= 0) goto done;
    if (devgroup_profile_read(&local, &lp)) { rc = -1; goto done; }
    if (devgroup_profile_compare(&bp, &lp) <= 0) { rc = 0; goto done; }
    if (memcmp(local.x, best.x, 32)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup: encryption key change rejected"); rc = -1; goto done; }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup: adopt newer profile version=%llu->%llu timestamp=%llu",
        (unsigned long long)lp.version, (unsigned long long)bp.version, (unsigned long long)bp.timestamp);
    rc = cd_rebind(inst, &best, ep);
    if (!rc) rc = cd_commit(inst, &best, ep, xp, NULL, 0);
done:
    OPENSSL_cleanse(ep, 32); OPENSSL_cleanse(xp, 32); return rc < 0 ? -1 : 0;
}
int chat_devgroup_rename(struct UTUN_INSTANCE* inst, const char* name) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: RW rename requires rename_submit"); return -1;
    }
    struct devgroup_record r; struct devgroup_profile p = {0}; uint8_t ep[32], xp[32];
    if (chat_devgroup_refresh(inst)) return -1;
    int rc = devgroup_load_local(inst, &r, ep, xp); if (rc < 0) goto done;
    if (rc) {
        if (devgroup_profile_read(&r, &p)) { rc = -1; goto done; }
        if (name && !strcmp(name, p.name)) { rc = 0; goto done; }
    } else if (RAND_bytes(ep, 32) != 1 || RAND_bytes(xp, 32) != 1 || devgroup_public_keys(ep, xp, r.ed, r.x)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: key generation failed"); rc = -1; goto done;
    }
    time_t now = ntp_time_get_seconds(inst);
    rc = devgroup_sign(&r, ep, name, p.version + 1, now > 0 ? (uint64_t)now : 1, inst->my_keys.public_key, inst->my_ed25519_pubkey);
    if (!rc) rc = cd_commit(inst, &r, ep, xp, NULL, 0);
done:
    OPENSSL_cleanse(ep, 32); OPENSSL_cleanse(xp, 32); return rc;
}
int chat_devgroup_leave(struct UTUN_INSTANCE* inst) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: RW leave requires leave_submit"); return -1;
    }
    struct devgroup_record r = {0}; uint8_t ep[32], xp[32];
    int found = devgroup_load_local(inst, &r, ep, xp);
    OPENSSL_cleanse(ep, 32); OPENSSL_cleanse(xp, 32);
    if (found < 0) return -1;
    int rc = 0;
    if (found) { memset(&r, 0, sizeof(r)); rc = cd_commit(inst, &r, NULL, NULL, NULL, 0); }
    if (!rc) chat_devgroup_cancel_pending(inst);
    return rc;
}
int chat_devgroup_import(struct UTUN_INSTANCE* inst, const struct devgroup_record* input,
    const uint8_t ep[32], const uint8_t xp[32], const uint8_t receipt[16], const uint8_t peer[32]) {
    if (!input || !ep || !xp || !receipt || (inst && db_async_can_write(inst->db_worker))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: invalid import or RW caller needs import_submit"); return -1;
    }
    int done = chat_devgroup_receipt(inst, receipt, peer); if (done) return done > 0 ? 0 : -1;
    struct devgroup_record local, r = *input; uint8_t old_e[32], old_x[32], ed[32], x[32]; struct devgroup_profile p;
    int found = devgroup_load_local(inst, &local, old_e, old_x);
    OPENSSL_cleanse(old_e, 32); OPENSSL_cleanse(old_x, 32);
    if (found < 0 || (found && (memcmp(local.ed, r.ed, 32) || memcmp(local.x, r.x, 32)))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup: import into another group rejected"); return -1;
    }
    if (devgroup_public_keys(ep, xp, ed, x) || memcmp(ed, r.ed, 32) || memcmp(x, r.x, 32) || devgroup_profile_read(&r, &p)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup: imported keys/profile mismatch"); return -1;
    }
    if (found) { struct devgroup_profile own; if (devgroup_profile_read(&local, &own)) return -1;
        if (devgroup_profile_compare(&own, &p) > 0) r = local;
    }
    if (cd_rebind(inst, &r, ep)) return -1;
    return cd_commit(inst, &r, ep, xp, receipt, peer);
}

int chat_devgroup_execute(const struct chat_devgroup_request* req, char** json) {
    if (!json) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: missing command result"); return -1; }
    *json = NULL;
    if (!req || !req->inst || !memchr(req->text, 0, sizeof(req->text)) || !memchr(req->pin, 0, sizeof(req->pin))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: invalid command"); return -1;
    }
    int rc = -1;
    switch (req->action) {
    case CHAT_DEVGROUP_STATUS: break;
    case CHAT_DEVGROUP_INVITE: {
        char id[33], pin[5]; struct strbuf result = {0};
        rc = chat_devgroup_invite(req->inst, topo_groups_channel_slot(req->inst->topo_groups, req->group), req->peer, id, pin);
        if (!rc && strbuf_addf(&result, "{\"id\":\"%s\",\"pin\":\"%s\"}", id, pin) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: invite result allocation failed"); rc = -1;
        }
        OPENSSL_cleanse(pin, sizeof(pin));
        if (!rc) *json = strbuf_detach(&result);
        strbuf_free(&result); return rc;
    }
    case CHAT_DEVGROUP_ACCEPT: rc = chat_devgroup_accept(req->inst, req->text, req->pin); break;
    case CHAT_DEVGROUP_RENAME: rc = chat_devgroup_rename(req->inst, req->text); break;
    case CHAT_DEVGROUP_LEAVE: rc = chat_devgroup_leave(req->inst); break;
    default: DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup: unknown action=%d", req->action); return -1;
    }
    if (req->action != CHAT_DEVGROUP_STATUS && rc) return -1;
    *json = chat_devgroup_status(req->inst);
    return *json ? 0 : -1;
}
struct cd_execute {
    struct chat_devgroup_request request; chat_devgroup_execute_done_fn done; void* arg;
};
static void cd_execute_done(void* data, const struct chat_devgroup_result* result) {
    struct cd_execute* job = data; char* json = result->status ? NULL : chat_devgroup_status(job->request.inst);
    job->done(job->arg, result->status || !json ? -1 : 0, json);
    if (json) { OPENSSL_cleanse(json, strlen(json)); u_free(json); }
    OPENSSL_cleanse(job, sizeof(*job)); u_free(job);
}
void chat_devgroup_execute_submit(const struct chat_devgroup_request* request, chat_devgroup_execute_done_fn done, void* arg) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: execute requires completion"); return; }
    if (!request || !request->inst || !memchr(request->text, 0, sizeof(request->text)) || !memchr(request->pin, 0, sizeof(request->pin))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: invalid copied execute request"); done(arg, -1, NULL); return;
    }
    struct cd_execute* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: copied command allocation failed"); done(arg, -1, NULL); return; }
    job->request = *request; job->done = done; job->arg = arg;
    if (db_async_can_write(request->inst->db_worker) && request->action == CHAT_DEVGROUP_RENAME) {
        chat_devgroup_rename_submit(request->inst, request->text, cd_execute_done, job); return;
    }
    if (db_async_can_write(request->inst->db_worker) && request->action == CHAT_DEVGROUP_LEAVE) {
        chat_devgroup_leave_submit(request->inst, cd_execute_done, job); return;
    }
    char* json = NULL; int rc = chat_devgroup_execute(&job->request, &json);
    done(arg, rc, json);
    if (json) { OPENSSL_cleanse(json, strlen(json)); u_free(json); }
    OPENSSL_cleanse(job, sizeof(*job)); u_free(job);
}
static void cd_gui_done(void* arg, int rc, const char* json) {
    struct chat_devgroup_request* req = arg;
    size_t n = json ? strlen(json) : 0; uint8_t* event = u_malloc(9 + n);
    if (event) {
        for (unsigned i = 0; i < 8; i++) event[i] = (uint8_t)(req->request_id >> (8 * i));
        event[8] = !rc; if (n) memcpy(event + 9, json, n);
        chat_event_post(req->inst, CHAT_EVT_DEVGROUP_RESULT, event, 9 + n);
        OPENSSL_cleanse(event, 9 + n); u_free(event);
    } else DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: result event allocation failed");
    OPENSSL_cleanse(req, sizeof(*req)); u_free(req);
}
void chat_devgroup_trampoline(void* arg) {
    struct chat_devgroup_request* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: missing request"); return; }
    chat_devgroup_execute_submit(req, cd_gui_done, req);
}
