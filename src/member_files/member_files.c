#include "private.h"
#include "../utun_instance.h"
#include "../chat/chat_event.h"
#include "../chat/chat_setting.h"
#include "../chat/group_policy.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/conn_mgr.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/json_flat.h"
#include "../../lib/db_async.h"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>

#define MF_MAX_PENDING 16
#define MF_MAX_JOBS 32

struct mf_pending {
    struct mf_pending* next;
    struct member_files* files;
    uint64_t group, started, sent, retry_at;
    uint8_t server[MF_ADDRESS_SIZE];
    int busy; // Уже залогировано ожидание квитанции; общий срок запроса не продлевается.
    struct mf_message request;
    mf_response_fn callback;
    void* arg;
    void* pump;
    void* retry;
    struct CONN_MGR_HANDLE* cm;
    struct queue_waiter_handle waiter;
};
struct mf_job {
    char group_key[65];
    struct mf_job* next;
    struct member_files* files;
    uint64_t group, created;
    uint8_t peer[MF_ADDRESS_SIZE];
    struct mf_message request, response;
    struct queue_waiter_handle waiter;
    struct mf_roots_job* roots; /* Слабое владение READ виртуального корня; free/cancel отсоединяет callback. */
    int state; /* 0 queued, 1 disk worker, 2 reply, 3 DB snapshot; command=0 — sweep */
    int cancelled; /* Uasync: результат worker больше не выдаётся; память живёт до done. */
};
struct mf_roots_sql {
    char channel[65]; uint64_t group; struct mf_message request, response; };
struct mf_roots_job {
    struct mf_job* owner; /* Только uasync; DB worker читает frozen sql, не runtime. */
    struct UTUN_INSTANCE* inst;
    uint64_t generation;
    struct mf_roots_sql sql;
};
struct mf_local {
    struct mf_local* next;
    struct member_files* files;
    uint64_t group;
    struct mf_message message;
    void* task;
};

static void mf_server_pump(struct member_files* f);
static void mf_client_ready(struct ll_queue* q, void* arg);
static void mf_dispatch(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* message);
static int mf_message_ready(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], int kind, int command, uint64_t id) {
    if (!f || f->closing) return 0;
    if (command == MF_STREAM_DATA) return mf_transfer_receive_ready(f, group, peer, kind, id);
    return kind != MF_REQUEST || (command > MF_STREAM_READ && command != MF_QUOTA) || f->job_count < MF_MAX_JOBS;
}
static int mf_receive_ready(const struct ETCP_ROUTER_CONN* route, const uint8_t* payload, size_t bytes) {
    if (bytes < MF_WIRE_HEADER) return 1; // Ошибку формата логирует декодер сервиса.
    return mf_message_ready(route->inst->member_files, route->group_id, route->remote_ed, payload[1], payload[2], mf_get64(payload + 5));
}
static int mf_local_ready(struct mf_local* local) {
    struct mf_message* m = &local->message;
    return mf_message_ready(local->files, local->group, local->files->inst->my_ed25519_pubkey, m->kind, m->command, m->request_id);
}

static void mf_local_deliver(void* arg);

void mf_receive_resume(struct member_files* f) {
    if (f->closing) return;
    for (struct ll_entry* e = f->inst->router_conns ? f->inst->router_conns->head : NULL; e; e = e->next) {
        struct ETCP_ROUTER_CONN* rc = (struct ETCP_ROUTER_CONN*)e;
        if (rc->svc_id == ETCP_RT_ID_MEMBER_FILES && rc->receive_blocked) etcp_router_conn_resume_receive(rc);
    }
    struct mf_local* local = f->locals;
    if (local && !local->task && mf_local_ready(local)) {
        local->task = uasync_call_soon(f->inst->ua, local, mf_local_deliver);
        if (!local->task) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: local delivery scheduling failed");
    }
}

static void mf_local_free(struct mf_local* local) {
    struct member_files* f = local->files;
    if (local->task) uasync_call_soon_cancel(f->inst->ua, local->task);
    struct mf_local** link = &f->locals;
    while (*link && *link != local) link = &(*link)->next;
    if (*link) *link = local->next;
    f->local_count--; u_free(local);
}

/* Локальная доставка всегда отложена: ответ не освобождает pending внутри send. */
static void mf_local_deliver(void* arg) {
    struct mf_local* local = arg; local->task = NULL;
    struct member_files* f = local->files;
    if (!mf_local_ready(local)) return;
    struct mf_local** link = &f->locals;
    while (*link && *link != local) link = &(*link)->next;
    if (*link) *link = local->next;
    f->local_count--;
    if (!f->closing) mf_dispatch(f, local->group, f->inst->my_ed25519_pubkey, &local->message);
    u_free(local);
    mf_receive_resume(f); mf_transfers_wake(f);
}

int member_files_member(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t node[MF_ADDRESS_SIZE], int storage) {
    struct TOPO_GROUP* g = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group) : NULL;
    if (!g || g->group_type != TOPO_GROUP_TYPE_CHAT || g->stopping || !node || mf_zero(node, MF_ADDRESS_SIZE)) {
        DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "member_files: ACL missing CHAT group=%llu node=%s require_storage=%d",
                    (unsigned long long)group, sc_public_key_hex(node, (char[65]){0}), storage); return 0;
    }
    if (db_async_can_write(inst->db_worker))
        return topo_group_chat_key_allowed(inst, group, node) &&
            (!storage || group_policy_member_role_key(inst, g->channel_id, node, GROUP_MEMBER_STORAGE) == 1);
    if (!inst->topo_sqlite_db) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: legacy ACL database absent"); return 0; }
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT adm_tags,source,adm_tags_sig,deleted FROM \"peers_%s\" WHERE ed25519_pubkey=? AND record_state=0 AND admitted=1",
             g->channel_id);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: membership query failed group=%llu: %s",
                    (unsigned long long)group, sqlite3_errmsg(inst->topo_sqlite_db)); return 0;
    }
    const char* stage = "bind"; int rc = sqlite3_bind_blob(st, 1, node, MF_ADDRESS_SIZE, SQLITE_STATIC);
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    int source = -1, deleted = -1, signature = 0, storage_role = 0, allowed = 0;
    if (rc == SQLITE_ROW) {
        char role[16]; const char* tags = (const char*)sqlite3_column_text(st, 0);
        source = sqlite3_column_int(st, 1); deleted = sqlite3_column_int(st, 3); signature = sqlite3_column_bytes(st, 2);
        storage_role = tags && json_flat_get(tags, "storage", role, sizeof(role)) == 0 && !strcmp(role, "yes");
        allowed = !source && !deleted && (!storage || (storage_role && signature == 64));
    }
    DEBUG_TRACE(DEBUG_CATEGORY_MEDIA,
        "member_files: ACL allowed=%d group=%llu node=%s require_storage=%d row=%d storage=%d source=%d signature=%d deleted=%d",
        allowed, (unsigned long long)group, sc_public_key_hex(node, (char[65]){0}), storage, rc == SQLITE_ROW,
        storage_role, source, signature, deleted);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA,
        "member_files: access check failed group=%llu node=%s require_storage=%d stage=%s rc=%d (%s): %s",
        (unsigned long long)group, sc_public_key_hex(node, (char[65]){0}), storage, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(inst->topo_sqlite_db));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA,
        "member_files: access finalize failed group=%llu node=%s sqlite_rc=%d: %s",
        (unsigned long long)group, sc_public_key_hex(node, (char[65]){0}), final, sqlite3_errmsg(inst->topo_sqlite_db));
    return allowed && final == SQLITE_OK;
}

/* Собственный сервер получает исключение только для канонического пространства аватарок. */
int mf_avatar_self_request(const uint8_t server[MF_ADDRESS_SIZE], const struct mf_message* q) {
    if (!server || memcmp(q->owner, server, MF_ADDRESS_SIZE) || q->command == MF_RENAME || q->command == MF_QUOTA) return 0;
    if (!strcmp(q->path, "avatars")) return q->command == MF_LIST || q->command == MF_STAT || q->command == MF_MKDIR;
    return !strncmp(q->path, "avatars/", 8) && mf_avatar_file_valid(q->path + 8);
}

int mf_storage_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t server[MF_ADDRESS_SIZE], const struct mf_message* q) {
    return member_files_member(inst, group, server, 0) &&
        (mf_avatar_self_request(server, q) || member_files_member(inst, group, server, 1));
}

int mf_server_allowed(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* q) {
    int enabled = chat_setting_get_int(f->inst, "member_files_enabled", 1);
    int allowed = enabled && mf_storage_allowed(f->inst, group, f->inst->my_ed25519_pubkey, q) &&
                  member_files_member(f->inst, group, peer, 0) && (mf_zero(q->owner, MF_ADDRESS_SIZE) || member_files_member(f->inst, group, q->owner, 0)) &&
                  (q->command < MF_BEGIN_WRITE || q->command == MF_STREAM_READ || q->command == MF_QUOTA ||
                   (!memcmp(q->owner, peer, MF_ADDRESS_SIZE)));
    return allowed;
}

/* До выдачи результата проверяем членство и право сервера обслуживать этот каталог. */
static int mf_client_allowed(struct member_files* f, uint64_t group, const uint8_t server[MF_ADDRESS_SIZE], const struct mf_message* q) {
    return member_files_member(f->inst, group, f->inst->my_ed25519_pubkey, 0) && mf_storage_allowed(f->inst, group, server, q) &&
        (mf_zero(q->owner, MF_ADDRESS_SIZE) || member_files_member(f->inst, group, q->owner, 0));
}

/* Router подтверждает доставку отдельно; здесь отправка всегда соблюдает порог очереди. */
int mf_send(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* m, struct ETCP_ROUTER_CONN** blocked) {
    *blocked = NULL;
    if (!topo_group_chat_key_allowed(f->inst, group, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: send denied group=%llx peer=%s cmd=%u id=%llu",
            (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), m->command, (unsigned long long)m->request_id); return -1;
    }
    if (!memcmp(peer, f->inst->my_ed25519_pubkey, MF_ADDRESS_SIZE)) {
        if (f->local_count >= MF_MAX_JOBS) return 1;
        struct mf_local* local = u_calloc(1, sizeof(*local));
        if (!local) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: local delivery allocation failed"); return -1; }
        local->files = f; local->group = group; local->message = *m;
        struct mf_local** tail = &f->locals; while (*tail) tail = &(*tail)->next;
        *tail = local; f->local_count++;
        mf_receive_resume(f);
        return 0;
    }
    struct ETCP_ROUTER_CONN* rc = etcp_router_conn_get_key(f->inst, group, peer, ETCP_RT_ID_MEMBER_FILES);
    if (!rc) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: router allocation failed"); return -1; }
    queue_set_threshold(rc->send_q, 0, 0);
    if (!etcp_router_conn_send_q_has_room(rc)) {
        *blocked = rc; return 1;
    }
    uint8_t wire[MF_WIRE_MAX];
    int n = mf_encode(m, wire, sizeof(wire));
    if (n < 0) return -1;
    int result = etcp_router_conn_send(rc, wire, (size_t)n, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED);
    if (result) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: send failed group=%llu peer=%s cmd=%u rc=%d",
                           (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), m->command, result);
    return result ? -1 : 0;
}

static void mf_pending_finish(struct mf_pending* p, const struct mf_message* response, int status) {
    struct member_files* f = p->files;
    if (p->pump) uasync_call_soon_cancel(f->inst->ua, p->pump);
    if (p->retry) uasync_cancel_timeout(f->inst->ua, p->retry);
    etcp_router_cancel_send_ready(&p->waiter);
    if (p->cm) conn_mgr_close(p->cm);
    struct mf_pending** link = &f->pending;
    while (*link && *link != p) link = &(*link)->next;
    if (*link) *link = p->next;
    f->pending_count--;
    struct mf_message result;
    if (!response) { result = p->request; result.kind = MF_RESPONSE; result.status = (uint16_t)status; result.data_len = 0; response = &result; }
    mf_response_fn cb = p->callback; void* arg = p->arg;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: request done id=%llu cmd=%s status=%s pending=%u elapsed_ms=%llu",
        (unsigned long long)p->request.request_id, mf_command_text(p->request.command), mf_status_text(response->status),
        f->pending_count, (unsigned long long)((get_time_tb() - p->started) / 10));
    u_free(p); cb(arg, response);
}

static void mf_pending_pump(void* arg) {
    struct mf_pending* p = arg; p->pump = NULL;
    if (p->files->closing) return;
    if (!mf_client_allowed(p->files, p->group, p->server, &p->request)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: pending access revoked group=%llx server=%s id=%llu",
            (unsigned long long)p->group, sc_public_key_hex(p->server, (char[65]){0}), (unsigned long long)p->request.request_id);
        mf_pending_finish(p, NULL, MF_ACCESS_DENIED); return;
    }
    if (p->retry) { uasync_cancel_timeout(p->files->inst->ua, p->retry); p->retry = NULL; }
    if (get_time_tb() - p->started >= MF_REPLAY_TB) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: request expired before send id=%llu cmd=%s",
            (unsigned long long)p->request.request_id, mf_command_text(p->request.command));
        mf_pending_finish(p, NULL, MF_TIMEOUT); return;
    }
    struct ETCP_ROUTER_CONN* blocked;
    int rc = mf_send(p->files, p->group, p->server, &p->request, &blocked);
    if (!rc) { p->sent = get_time_tb(); p->retry_at = 0; }
    else if (blocked) etcp_router_conn_on_send_ready(blocked, &p->waiter, mf_client_ready, p);
}

static void mf_pending_wake(struct mf_pending* p) {
    if (p->pump || p->files->closing || get_time_tb() < p->retry_at) return;
    p->pump = uasync_call_soon(p->files->inst->ua, p, mf_pending_pump);
    if (!p->pump) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: client pump allocation failed");
}

static void mf_client_ready(struct ll_queue* q, void* arg) { (void)q; mf_pending_wake(arg); }

/* Отдельный таймер не округляет срок освобождения квитанции до секундного sweep. */
static void mf_pending_retry(void* arg) {
    struct mf_pending* p = arg; p->retry = NULL; p->retry_at = 0;
    mf_pending_wake(p);
}

int mf_open_cm(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[MF_ADDRESS_SIZE],
               conn_mgr_cb_t cb, void* arg, struct CONN_MGR_HANDLE** handle) {
    if (!memcmp(ed, inst->my_ed25519_pubkey, MF_ADDRESS_SIZE)) return 0;
    return conn_mgr_open(inst, group, ed, cb, arg, handle);
}

static void mf_connected(struct CONN_MGR_HANDLE* h, const uint8_t peer[32], uint64_t group, enum conn_mgr_event event, void* arg) {
    (void)h; (void)peer; (void)group;
    struct mf_pending* p = arg;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: CM event=%d request=%llu", event, (unsigned long long)p->request.request_id);
    if (event == CONN_EVENT_UP) mf_pending_wake(p);
}

struct mf_pending* member_files_request(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t server[MF_ADDRESS_SIZE],
                                       const struct mf_message* request, mf_response_fn cb, void* arg) {
    struct member_files* f = inst ? inst->member_files : NULL;
    if (request && (request->command < MF_LIST || (request->command > MF_STREAM_READ && request->command != MF_QUOTA) || request->status ||
        request->data_len > MF_BLOCK_SIZE || !mf_path_valid(request->path,
            request->command == MF_LIST || request->command == MF_STAT || request->command == MF_QUOTA) ||
        (request->command == MF_RENAME && !mf_path_valid(request->target, 0)))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: invalid client command=%u status=%u data=%u",
                   request->command, request->status, request->data_len); return NULL;
    }
    if (!f || f->closing || !request || !cb || f->pending_count >= MF_MAX_PENDING ||
        !mf_client_allowed(f, group, server, request)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: client request denied group=%llu server=%s",
                   (unsigned long long)group, sc_public_key_hex(server, (char[65]){0})); return NULL;
    }
    struct mf_pending* p = u_calloc(1, sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: client allocation failed"); return NULL; }
    p->files = f; p->group = group; memcpy(p->server, server, MF_ADDRESS_SIZE); p->request = *request; p->request.kind = MF_REQUEST;
    p->callback = cb; p->arg = arg; p->started = get_time_tb();
    if (!p->request.request_id && (RAND_bytes((uint8_t*)&p->request.request_id, 8) != 1 || !p->request.request_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: request random failed"); u_free(p); return NULL;
    }
    for (struct mf_pending* other = f->pending; other; other = other->next) if (other->request.request_id == p->request.request_id) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: duplicate local request id"); u_free(p); return NULL;
    }
    p->next = f->pending; f->pending = p; f->pending_count++;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA,
        "member_files: request id=%llu cmd=%s group=%llx server=%s owner=%s path=%s offset=%llu size=%llu pending=%u",
        (unsigned long long)p->request.request_id, mf_command_text(p->request.command), (unsigned long long)group,
        sc_public_key_hex(server, (char[65]){0}), sc_public_key_hex(p->request.owner, (char[65]){0}), p->request.path,
        (unsigned long long)p->request.offset, (unsigned long long)p->request.size, f->pending_count);
    if (mf_open_cm(inst, group, server, mf_connected, p, &p->cm) != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: CM launch failed server=%s", sc_public_key_hex(server, (char[65]){0}));
        if (p->pump) uasync_call_soon_cancel(inst->ua, p->pump);
        f->pending = p->next; f->pending_count--; u_free(p); return NULL;
    }
    mf_pending_wake(p);
    return p;
}

void member_files_cancel(struct mf_pending* p) { if (p) mf_pending_finish(p, NULL, MF_CANCELLED); }

void member_files_cancel_request_id(struct UTUN_INSTANCE* inst, uint64_t id) {
    struct member_files* f = inst ? inst->member_files : NULL;
    if (f) for (struct mf_pending* p = f->pending; p; p = p->next) if (p->request.request_id == id) {
        member_files_cancel(p); break;
    }
}

static void mf_job_free(struct mf_job* j) {
    struct member_files* f = j->files;
    if (j->roots) { j->roots->owner = NULL; j->roots = NULL; }
    etcp_router_cancel_send_ready(&j->waiter);
    struct mf_job** link = &f->jobs;
    while (*link && *link != j) link = &(*link)->next;
    if (*link) *link = j->next;
    f->job_count--; u_free(j);
    mf_receive_resume(f);
}

/* Worker владеет снимком до done; отмена очереди не вызывает completion синхронно. */
static void mf_job_cancel(struct mf_job* j) {
    struct member_files* f = j->files;
    j->cancelled = 1;
    etcp_router_cancel_send_ready(&j->waiter);
    unsigned queued = j == f->worker ? mf_worker_cancel(f->disk, j) : 0;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: cancel job group=%llx peer=%s id=%llu cmd=%s state=%d disk_queued=%u",
        (unsigned long long)j->group, sc_public_key_hex(j->peer, (char[65]){0}), (unsigned long long)j->request.request_id,
        mf_command_text(j->request.command), j->state, queued);
    if (j != f->worker) mf_job_free(j);
}

static void mf_server_deferred(void* arg) {
    struct member_files* f = arg; f->pump = NULL; mf_server_pump(f);
}

void mf_reply(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* q, int status) {
    struct mf_job* j = f->job_count < MF_MAX_JOBS ? u_calloc(1, sizeof(*j)) : NULL;
    if (!j) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: reply queue allocation/limit failure status=%s", mf_status_text(status)); return; }
    j->files = f; j->group = group; strcpy(j->group_key, topo_groups_channel(f->inst->topo_groups, group)); memcpy(j->peer, peer, MF_ADDRESS_SIZE); j->request = j->response = *q;
    j->response.kind = MF_RESPONSE; j->response.status = (uint16_t)status; j->response.data_len = 0;
    j->state = 2; j->created = get_time_tb();
    struct mf_job** tail = &f->jobs; while (*tail) tail = &(*tail)->next;
    *tail = j; f->job_count++;
    mf_server_pump(f);
}
static void mf_server_ready(struct ll_queue* q, void* arg) {
    (void)q;
    struct member_files* f = arg;
    if (!f->pump && !f->closing) {
        f->pump = uasync_call_soon(f->inst->ua, f, mf_server_deferred);
        if (!f->pump) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: server pump allocation failed");
    }
}

static void mf_server_work(void* arg) {
    struct mf_job* j = arg;
    if (j->request.command) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: disk command start group=%llx peer=%s id=%llu cmd=%u",
        (unsigned long long)j->group, sc_public_key_hex(j->peer, (char[65]){0}), (unsigned long long)j->request.request_id, j->request.command);
    if (j->request.command) mf_storage_execute(j->files->storage, j->group_key, j->peer, &j->request, &j->response);
    else mf_storage_sweep(j->files->storage, get_time_tb());
}

static void mf_server_done(void* arg, int error) {
    struct mf_job* j = arg;
    struct member_files* f = j->files; f->worker = NULL;
    if (f->closing || j->cancelled || !j->request.command) { mf_job_free(j); if (!f->closing) mf_server_pump(f); return; }
    if (error) { j->response = j->request; j->response.kind = MF_RESPONSE; j->response.status = MF_BUSY; j->response.data_len = 0; }
    if (error) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: disk job failed id=%llu cmd=%s error=%d jobs=%u",
        (unsigned long long)j->request.request_id, mf_command_text(j->request.command), error, f->job_count);
    j->state = 2; mf_server_pump(f);
}

/* Виртуальный корень — действующие участники, в том числе ещё без файлов на диске. */
static int mf_roots_read(sqlite3* db, void* arg) {
    struct mf_roots_sql* data = arg; struct mf_message* r = &data->response;
    *r = data->request; r->kind = MF_RESPONSE; r->type = MF_DIRECTORY; r->data_len = 0; r->more = 0;
    if (*r->path || r->command != MF_LIST) { r->status = MF_INVALID_PATH; return SQLITE_OK; }
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey,join_ts,update_ts,adm_tags FROM \"peers_%s\""
        " WHERE record_state=0 AND deleted=0 AND source=0 AND admitted=1 ORDER BY ed25519_pubkey",
             data->channel);
    sqlite3_stmt* st = NULL; EVP_MD_CTX* hash = EVP_MD_CTX_new();
    int rc = !hash ? SQLITE_NOMEM : EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1 ? SQLITE_ERROR : SQLITE_OK;
    if (!rc) rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc) goto done;
    uint64_t seen = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        uint8_t metadata[48];
        const uint8_t* ed = sqlite3_column_blob(st, 0);
        if (!ed || sqlite3_column_bytes(st, 0) != MF_ADDRESS_SIZE) { rc = SQLITE_CORRUPT; goto done; }
        memcpy(metadata, ed, MF_ADDRESS_SIZE);
        mf_put64(metadata + 32, (uint64_t)sqlite3_column_int64(st, 1));
        mf_put64(metadata + 40, (uint64_t)sqlite3_column_int64(st, 2));
        const char* tags = (const char*)sqlite3_column_text(st, 3);
        if (EVP_DigestUpdate(hash, metadata, sizeof(metadata)) != 1 ||
            (tags && EVP_DigestUpdate(hash, tags, strlen(tags) + 1) != 1)) { rc = SQLITE_ERROR; goto done; }
        if (seen++ < data->request.cursor || r->more) continue;
        char name[65]; sc_public_key_hex(ed, name); int n = 64;
        if (r->data_len + MF_ENTRY_HEADER + (unsigned)n > MF_BLOCK_SIZE) { r->more = 1; continue; }
        uint8_t* p = r->data + r->data_len;
        memset(p, 0, MF_ENTRY_HEADER); p[0] = (uint8_t)n; p[2] = MF_DIRECTORY;
        memcpy(p + MF_ENTRY_HEADER, name, (size_t)n); r->data_len += MF_ENTRY_HEADER + (uint32_t)n; r->cursor++;
    }
    uint8_t digest[32]; unsigned n;
    if (rc != SQLITE_DONE) goto done;
    rc = EVP_DigestFinal_ex(hash, digest, &n) != 1 || n != 32 ? SQLITE_ERROR : SQLITE_OK;
    if (rc) goto done;
    memcpy(r->version, digest, 16);
    if ((data->request.cursor && mf_zero(data->request.version, 16)) ||
        (!mf_zero(data->request.version, 16) && memcmp(data->request.version, r->version, 16))) r->status = MF_STALE;
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: virtual root cleanup finalize rc=%d", final); }
    EVP_MD_CTX_free(hash);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: virtual root group=%llx rc=%d: %s",
            (unsigned long long)data->group, rc, sqlite3_errmsg(db)); r->status = MF_IO_ERROR;
    }
    if (r->status) r->data_len = 0;
    return rc;
}
static int mf_roots_current(void* arg) {
    struct mf_roots_job* job = (void*)((char*)arg - offsetof(struct mf_roots_job, sql));
    struct mf_job* owner = job->owner;
    return owner && !owner->cancelled && !owner->files->closing && !job->inst->destroying && job->inst->chat_content_started &&
        job->inst->group_generation == job->generation && mf_server_allowed(owner->files, owner->group, owner->peer, &owner->request);
}
static void mf_roots_done(void* arg, const struct db_async_result* result) {
    struct mf_roots_job* job = (void*)((char*)arg - offsetof(struct mf_roots_job, sql)); struct mf_job* owner = job->owner;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: root snapshot status=%d rc=%d current=%d: %s",
        result->status, result->sqlite_code, mf_roots_current(arg), result->message);
    if (!owner) return;
    struct member_files* files = owner->files; owner->roots = NULL; job->owner = NULL;
    if (owner->cancelled || files->closing || job->inst->destroying || !job->inst->chat_content_started ||
        job->inst->group_generation != job->generation) {
        mf_job_free(owner); return;
    }
    owner->response = job->sql.response;
    if (result->status) {
        owner->response = owner->request; owner->response.kind = MF_RESPONSE; owner->response.data_len = 0;
        owner->response.status = result->status == DB_ASYNC_FULL || result->status == DB_ASYNC_NOMEM ? MF_BUSY : MF_IO_ERROR;
    }
    owner->state = 2; mf_server_pump(files);
}
static void mf_roots_cleanup(void* arg) { u_free((char*)arg - offsetof(struct mf_roots_job, sql)); }
static void mf_roots(struct mf_job* owner) {
    struct UTUN_INSTANCE* inst = owner->files->inst;
    if (!db_async_can_write(inst->db_worker)) {
        struct mf_roots_sql data = {.group=owner->group, .request=owner->request};
        strcpy(data.channel, topo_groups_channel(owner->files->inst->topo_groups, owner->group));
        mf_roots_read(inst->topo_sqlite_db, &data); owner->response = data.response; owner->state = 2; return;
    }
    struct mf_roots_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: root snapshot allocation failed");
        owner->response = owner->request; owner->response.kind = MF_RESPONSE; owner->response.status = MF_BUSY;
        owner->response.data_len = 0; owner->state = 2; return;
    }
    job->owner = owner; job->inst = inst; job->generation = inst->group_generation;
    job->sql.group = owner->group;
    strcpy(job->sql.channel, topo_groups_channel(job->owner->files->inst->topo_groups, job->sql.group)); job->sql.request = owner->request; owner->roots = job; owner->state = 3;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="member files roots", .mode=DB_ASYNC_READ, .data=&job->sql,
        .guard=mf_roots_current, .work=mf_roots_read, .done=mf_roots_done, .cleanup=mf_roots_cleanup});
}

/* Один disk worker: I/O, sweep и namespace-мутации никогда не пересекаются. */
static void mf_server_pump(struct member_files* f) {
    if (f->closing) return;
    struct mf_job* j = f->jobs;
    while (j) {
        struct mf_job* next = j->next;
        if (j->state == 2 && !j->waiter.internal) {
            if (j->response.status == MF_OK && !mf_server_allowed(f, j->group, j->peer, &j->request)) {
                j->response.status = MF_ACCESS_DENIED; j->response.data_len = 0;
            }
            struct ETCP_ROUTER_CONN* blocked;
            int result = mf_send(f, j->group, j->peer, &j->response, &blocked);
            if (result <= 0) mf_job_free(j);
            else if (blocked) etcp_router_conn_on_send_ready(blocked, &j->waiter, mf_server_ready, f);
        }
        j = next;
    }
    if (f->worker) return;
    for (j = f->jobs; j; j = j->next) if (!j->state) break;
    if (!j && f->sweep_due && f->job_count < MF_MAX_JOBS) {
        j = u_calloc(1, sizeof(*j));
        if (!j) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: sweep allocation failed"); return; }
        j->files = f; j->next = f->jobs; f->jobs = j; f->job_count++; f->sweep_due = 0;
    }
    if (!j) return;
    if (j->request.command && !mf_server_allowed(f, j->group, j->peer, &j->request)) {
        j->response = j->request; j->response.kind = MF_RESPONSE; j->response.status = MF_ACCESS_DENIED;
        j->response.data_len = 0; j->state = 2; mf_server_pump(f); return;
    }
    j->state = 1; f->worker = j;
    if (j->request.command) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: disk command queued group=%llx peer=%s id=%llu cmd=%u jobs=%u",
        (unsigned long long)j->group, sc_public_key_hex(j->peer, (char[65]){0}), (unsigned long long)j->request.request_id, j->request.command, f->job_count);
    mf_worker_submit(f->disk, mf_server_work, j, mf_server_done, j);
}

static void mf_tick(void* arg) {
    struct member_files* f = arg; f->tick = NULL;
    uint64_t now = get_time_tb();
    mf_transfers_tick(f, now);
    mf_receive_resume(f);
    for (;;) {
        struct mf_pending* expired = NULL;
        for (struct mf_pending* p = f->pending; p; p = p->next) if (now - p->started >= MF_REPLAY_TB) { expired = p; break; }
        if (!expired) break;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: timeout id=%llu cmd=%u",
                   (unsigned long long)expired->request.request_id, expired->request.command);
        mf_pending_finish(expired, NULL, MF_TIMEOUT);
    }
    for (struct mf_pending* p = f->pending; p; p = p->next)
        if (p->retry_at ? now >= p->retry_at : !p->sent || now - p->sent >= 50000) mf_pending_wake(p);
    struct mf_job* j = f->jobs;
    while (j) {
        struct mf_job* next = j->next;
        if (j != f->worker && j->created && now - j->created >= MF_REPLAY_TB) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: server reply/queue timeout peer=%s", sc_public_key_hex(j->peer, (char[65]){0})); mf_job_free(j);
        }
        j = next;
    }
    f->sweep_due = f->used_storage; mf_server_pump(f);
    f->tick = uasync_set_timeout(f->inst->ua, 10000, f, mf_tick, "member_files");
    if (!f->tick) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: timer allocation failed");
}

/* Общая обработка проверенного сетевого сообщения или команды внутри экземпляра. */
static void mf_dispatch(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* message) {
    if (!topo_group_chat_key_allowed(f->inst, group, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: delivery denied group=%llx peer=%s cmd=%u id=%llu",
            (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), message->command, (unsigned long long)message->request_id); return;
    }
    struct mf_message m = *message;
    if (m.kind == MF_REQUEST && f->storage_status != MF_OK) {
        int status = mf_server_allowed(f, group, peer, &m) ? f->storage_status : MF_ACCESS_DENIED;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: request before storage ready id=%llu status=%s",
            (unsigned long long)m.request_id, mf_status_text(status));
        mf_reply(f, group, peer, &m, status); return;
    }
    if (m.command >= MF_STREAM_WRITE && m.command <= MF_STREAM_CANCEL &&
        (m.kind == MF_REQUEST || m.command >= MF_STREAM_DATA)) {
        mf_transfer_dispatch(f, group, peer, &m);
        return;
    }
    if (m.kind == MF_RESPONSE) {
        for (struct mf_pending* p = f->pending; p; p = p->next) {
            if (p->group != group || memcmp(p->server, peer, MF_ADDRESS_SIZE) || p->request.request_id != m.request_id) continue;
            if (p->request.command != m.command || memcmp(p->request.owner, m.owner, MF_ADDRESS_SIZE) || strcmp(p->request.path, m.path)) {
                DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: mismatched response id=%llu", (unsigned long long)m.request_id); return;
            }
            if (!mf_client_allowed(f, group, peer, &p->request)) { m.status = MF_ACCESS_DENIED; m.data_len = 0; }
            if (m.status == MF_BUSY && (m.command == MF_COMMIT || (m.command >= MF_MKDIR && m.command <= MF_REMOVE_PATH))) {
                if (m.cursor > MF_REPLAY_TB) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: invalid busy hint id=%llu delay_tb=%llu; using5s",
                    (unsigned long long)m.request_id, (unsigned long long)m.cursor);
                uint64_t now = get_time_tb(), delay = m.cursor && m.cursor <= MF_REPLAY_TB ? m.cursor : 50000;
                p->retry_at = now + delay;
                p->sent = now;
                if (p->pump) { uasync_call_soon_cancel(f->inst->ua, p->pump); p->pump = NULL; }
                if (p->retry) uasync_cancel_timeout(f->inst->ua, p->retry);
                p->retry = uasync_set_timeout(f->inst->ua, (int)delay, p, mf_pending_retry, "member_files_retry");
                if (!p->retry) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: busy retry timer failed id=%llu; tick will retry",
                    (unsigned long long)m.request_id);
                etcp_router_cancel_send_ready(&p->waiter);
                if (!p->busy) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: mutation busy; retaining request id=%llu cmd=%s limit=120s",
                    (unsigned long long)m.request_id, mf_command_text(m.command));
                p->busy = 1;
                DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: busy retry id=%llu elapsed_ms=%llu delay_tb=%llu retry_at=%llu",
                    (unsigned long long)m.request_id, (unsigned long long)((now - p->started) / 10),
                    (unsigned long long)delay, (unsigned long long)p->retry_at);
                return;
            }
            mf_pending_finish(p, &m, 0); break;
        }
        return;
    }
    for (struct mf_job* other = f->jobs; other; other = other->next) if (other->group == group && !memcmp(other->peer, peer, MF_ADDRESS_SIZE) &&
        other->request.request_id == m.request_id) return;
    if (f->job_count >= MF_MAX_JOBS) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: server queue full peer=%s", sc_public_key_hex(peer, (char[65]){0})); return;
    }
    struct mf_job* j = u_calloc(1, sizeof(*j));
    if (!j) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: server job allocation failed"); return; }
    j->files = f; j->group = group; strcpy(j->group_key, topo_groups_channel(f->inst->topo_groups, group)); memcpy(j->peer, peer, MF_ADDRESS_SIZE); j->created = get_time_tb(); j->request = m;
    struct mf_job** tail = &f->jobs; while (*tail) tail = &(*tail)->next;
    *tail = j; f->job_count++;
    if (!mf_server_allowed(f, group, peer, &m)) {
        j->response = m; j->response.kind = MF_RESPONSE; j->response.status = MF_ACCESS_DENIED; j->response.data_len = 0; j->state = 2;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: ACL denied group=%llu peer=%s owner=%s cmd=%u",
                   (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), sc_public_key_hex(m.owner, (char[65]){0}), m.command);
    } else if (mf_zero(m.owner, MF_ADDRESS_SIZE)) mf_roots(j);
    else f->used_storage = 1;
    mf_server_pump(f);
}

static void mf_recv(struct ETCP_CONN* conn, struct ll_entry* e) {
    struct member_files* f = conn ? conn->instance->member_files : NULL;
    if (!f || f->closing || !e || !e->dgram || e->len < ROUTER_SVC_PAYLOAD_OFF) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: recv discarded context=%p closing=%d bytes=%u",
                    (void*)f, f ? f->closing : 0, e ? e->len : 0);
        goto out;
    }
    uint64_t group;
    memcpy(&group, e->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* peer = e->dgram + ROUTER_SVC_SRC_ED_OFF;
    if (e->len == ROUTER_SVC_PAYLOAD_OFF) {
        mf_transfers_peer_down(f, group, peer);
        member_files_revalidate_members(f->inst, group); goto out;
    }
    if ((e->dgram[ROUTER_SVC_FLAGS_OFF] & (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) !=
        (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: unsigned/unencrypted packet peer=%s", sc_public_key_hex(peer, (char[65]){0})); goto out;
    }
    struct mf_message m;
    if (!mf_decode(e->dgram + ROUTER_SVC_PAYLOAD_OFF, e->len - ROUTER_SVC_PAYLOAD_OFF, &m)) mf_dispatch(f, group, peer, &m);
out:
    if (e) { queue_dgram_free(e); queue_entry_free(e); }
}

static void mf_storage_start_work(void* arg) {
    struct member_files* f = arg;
    f->init_result = mf_storage_initialize(f->storage);
}

static void mf_storage_start_done(void* arg, int error) {
    struct member_files* f = arg;
    f->storage_status = error ? MF_IO_ERROR : f->init_result;
    if (f->storage_status) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: startup storage unavailable status=%s",
        mf_status_text(f->storage_status));
    else f->used_storage = 1;
    if (!f->closing) { mf_receive_resume(f); mf_server_pump(f); }
}

int member_files_init(struct UTUN_INSTANCE* inst) {
    if (!inst || inst->member_files) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: invalid init"); return -1; }
    struct member_files* f = u_calloc(1, sizeof(*f));
    if (!f) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: owner allocation failed"); return -1; }
    f->inst = inst;
    f->disk = mf_worker_create(inst->ua); f->cleanup = mf_worker_reserve();
    if (!f->disk || !f->cleanup) {
        mf_worker_destroy(f->disk); mf_worker_unreserve(f->cleanup); u_free(f); return -1;
    }
    char path[2048]; const char* custom = chat_setting_get(inst, "member_files_path");
    if (custom && !*custom) custom = NULL;
    snprintf(path, sizeof(path), "%s%s%s", custom ? custom : inst->config->global.db_path, custom ? "" : "/", custom ? "" : "member_files");
    if (!custom && !*inst->config->global.db_path) snprintf(path, sizeof(path), "member_files");
    f->storage = mf_storage_create(path, (uint64_t)chat_setting_get_int(inst, "member_files_user_quota_mb", 1024) * 1024 * 1024,
                                    (uint64_t)chat_setting_get_int(inst, "member_files_upload_ttl_sec", 3600) * 10000);
    if (!f->storage || etcp_router_bind(inst, ETCP_RT_ID_MEMBER_FILES, mf_recv)) {
        mf_storage_destroy(f->storage); mf_worker_destroy(f->disk); mf_worker_unreserve(f->cleanup); u_free(f); return -1;
    }
    inst->member_files = f;
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_MEMBER_FILES, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_MEMBER_FILES, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: router access policy initialization failed");
        member_files_destroy(inst); return -1;
    }
    f->storage_status = MF_BUSY;
    etcp_router_set_receive_ready(inst, ETCP_RT_ID_MEMBER_FILES, mf_receive_ready);
    f->tick = uasync_set_timeout(inst->ua, 10000, f, mf_tick, "member_files");
    if (!f->tick) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: init timer failed"); member_files_destroy(inst); return -1; }
    mf_worker_submit(f->disk, mf_storage_start_work, f, mf_storage_start_done, f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: initialized root=%s idle=10s max_requests=%d max_jobs=%d", path, MF_MAX_PENDING, MF_MAX_JOBS);
    return 0;
}

void member_files_quiesce(struct UTUN_INSTANCE* inst) {
    struct member_files* f = inst ? inst->member_files : NULL;
    if (!f || f->closing) return;
    f->closing = 1;
    if (f->pump) uasync_call_soon_cancel(inst->ua, f->pump);
    f->pump = NULL;
    if (f->tick) uasync_cancel_timeout(inst->ua, f->tick);
    f->tick = NULL;
    mf_transfers_quiesce(f, 0);
    while (f->pending) member_files_cancel(f->pending);
    while (f->locals) mf_local_free(f->locals);
    struct mf_job* j = f->jobs;
    while (j) { struct mf_job* next = j->next; mf_job_cancel(j); j = next; }
    etcp_router_unbind(inst, ETCP_RT_ID_MEMBER_FILES);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: quiesce workers=%d transfers=%u", f->worker != NULL, f->transfer_count);
}

static void mf_storage_cleanup(void* arg) { mf_storage_destroy(arg); }
static void mf_cleanup_done(void* arg, int error) { (void)arg; (void)error; }

void member_files_destroy(struct UTUN_INSTANCE* inst) {
    struct member_files* f = inst ? inst->member_files : NULL;
    if (!f) return;
    member_files_quiesce(inst);
    // Server streams закрывают temp/хеши в cleanup; storage должен оставаться жив до их завершения.
    mf_worker_drain(f->disk);
    mf_worker_submit_reserved(f->disk, f->cleanup, mf_storage_cleanup, f->storage, mf_cleanup_done, NULL);
    f->cleanup = NULL;
    mf_worker_destroy(f->disk);
    u_free(f); inst->member_files = NULL;
}

void member_files_cancel_group(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct member_files* f = inst ? inst->member_files : NULL;
    if (!f) return;
    mf_transfers_quiesce(f, group);
    struct mf_local* local = f->locals;
    while (local) { struct mf_local* next = local->next; if (local->group == group) mf_local_free(local); local = next; }
    for (;;) {
        struct mf_pending* found = NULL;
        for (struct mf_pending* p = f->pending; p; p = p->next) if (p->group == group) { found = p; break; }
        if (!found) break;
        member_files_cancel(found);
    }
    for (;;) {
        struct mf_job* found = NULL;
        for (struct mf_job* j = f->jobs; j; j = j->next) if (j->group == group && !j->cancelled) { found = j; break; }
        if (!found) break;
        mf_job_cancel(found);
    }
}

/* Поиск повторяется после каждого callback/resume: он может менять остальные запросы. */
void member_files_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct member_files* f = inst ? inst->member_files : NULL;
    if (!f || f->closing) return;
    for (;;) {
        struct mf_job* denied = NULL;
        for (struct mf_job* j = f->jobs; j; j = j->next)
            if (j->group == group && j->request.command && !j->cancelled && !mf_server_allowed(f, group, j->peer, &j->request)) {
                denied = j; break;
            }
        if (!denied) break;
        mf_job_cancel(denied);
    }
    mf_transfers_revalidate(f, group);
    for (;;) {
        struct mf_pending* denied = NULL;
        for (struct mf_pending* p = f->pending; p; p = p->next)
            if (p->group == group && !mf_client_allowed(f, group, p->server, &p->request)) { denied = p; break; }
        if (!denied) break;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: revoke request group=%llx server=%s id=%llu",
            (unsigned long long)group, sc_public_key_hex(denied->server, (char[65]){0}), (unsigned long long)denied->request.request_id);
        mf_pending_finish(denied, NULL, MF_ACCESS_DENIED);
    }
    struct mf_local* local = f->locals;
    while (local) {
        struct mf_local* next = local->next;
        if (local->group == group && !topo_group_chat_key_allowed(inst, group, inst->my_ed25519_pubkey)) mf_local_free(local);
        local = next;
    }
}

static void mf_gui_done(void* arg, const struct mf_message* response) {
    struct mf_gui_request* q = arg;
    uint8_t event[MF_EVENT_RESPONSE_PREFIX + MF_WIRE_MAX]; memcpy(event, q->group, MF_ADDRESS_SIZE);
    memcpy(event + MF_ADDRESS_SIZE, q->server, MF_ADDRESS_SIZE);
    int n = mf_encode(response, event + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX);
    if (n > 0) chat_event_post(q->inst, CHAT_EVT_MEMBER_FILES_RESPONSE, event, n + MF_EVENT_RESPONSE_PREFIX);
    u_free(q);
}

void member_files_gui_request(void* arg) {
    struct mf_gui_request* q = arg;
    struct TOPO_GROUP* group = q->inst ? topo_groups_find_wire(q->inst->topo_groups, q->group) : NULL;
    if (group && group->group_type == TOPO_GROUP_TYPE_CHAT &&
        member_files_request(q->inst, group->group_id, q->server, &q->message, mf_gui_done, q)) return;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: GUI request denied group=%s id=%llu",
        sc_public_key_hex(q->group, (char[65]){0}), (unsigned long long)q->message.request_id);
    q->message.kind = MF_RESPONSE; q->message.status = MF_ACCESS_DENIED; q->message.data_len = 0;
    mf_gui_done(q, &q->message);
}
