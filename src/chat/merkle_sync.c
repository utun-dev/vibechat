#include "merkle_sync.h"
#include "chat_join.h"
#include "chat_sync.h"
#include "../db_changes.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/node_conn_direct.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/ll_queue.h"
#include "../../lib/platform_compat.h"
#include "../../lib/db_async.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* Wire v5: svc:1, group_key:32, type:1, round:8, request:4, dataset:1,
 * round_id:16, accept_id:16, payload. round — диагностический счётчик;
 * пара случайных идентификаторов связывает запрос с текущим handshake обеих сторон.
 * Все целые заголовка big-endian. Один запрос на проход и один исходящий пакет на сессию.
 * Лексикографически меньший Ed25519 ведёт обычный раунд: pull -> TURN -> reverse pull -> CHECK -> DONE.
 * В одностороннем запросе получатель ведёт BEGIN -> pull -> CHECK -> DONE;
 * MEMBERS сверяет прочитанные листья с корнем и требует его подтверждения перед commit. */
#define MS_TOKEN_SIZE 16
#define MS_CONTROL_LIMIT 8u /* Команды одной сессии, ожидающие завершения её короткого SQL-шага. */
#define MS_QUERY_SIZE (10 + MT_KEY_SIZE)
#define MS_PAGE_HEADER (1 + MT_KEY_SIZE)
#define MS_PROGRESS_TIMEOUT_TB (30u * 10000u)
#define MS_ROUND_TIMEOUT_TB (600u * 10000u)
#define MS_SQL_RETRY_MIN_TB 250u
#define MS_SQL_RETRY_MAX_TB 5000u
#define MS_ROUND_RETRY_MIN_TB 10000u
#define MS_ROUND_RETRY_MAX_TB 300000u
/* В type два флага отделяют pull от обычного обмена и встречного pull того же пира. */
#define MS_READ_ONLY 0x40
#define MS_RESPONSE 0x80
enum { MS_WAKE = 0x10, MS_BEGIN, MS_READY, MS_QUERY, MS_HASHES, MS_PAGE, MS_TURN, MS_CHECK, MS_DONE, MS_ERROR,
    MS_DEP_QUERY, MS_DEP_REPLY, MS_SOURCE_CHECK, MS_SOURCE_OK };
enum { MS_IDLE, MS_BEGIN_WAIT, MS_PULL, MS_SERVE, MS_DONE_WAIT, MS_FAILED, MS_FAILING, MS_COMPLETE,
    MS_APPLY_WAIT, MS_READER_APPLY_WAIT, MS_MEMBERS_COMMITTED, MS_SOURCE_WAIT, MS_WAKE_WAIT };
enum { MS_EXCHANGE, MS_READER, MS_WRITER };

struct ms_request {
    struct ms_request* next;
    merkle_sync_done_cb cb;
    void* arg;
};

struct ms_frame {
    uint64_t prefix;
    uint32_t children;
    uint8_t level;
};

struct ms_dataset {
    const struct merkle_sync_data_ops* ops;
    void* data_ctx;
    uint8_t id;
};

struct ms_session {
    struct ms_session* next;
    struct merkle_sync* ms;
    struct ms_dataset* dataset;
    struct ETCP_CONN* conn;
    struct chat_join_path join_path; /* Для первого однонаправленного MEMBERS. */
    void* join_tx;
    struct NODE_CONN_DIRECT* transport; /* Владение только активным обменом, не постоянной подпиской. */
    struct ll_queue* send_q;
    struct queue_waiter_handle waiter;
    struct ll_entry* tx;
    void* work;
    void* deadline;
    uint64_t round_deadline_tb; /* От передачи/приёма BEGIN, не продлевается с прогрессом. */
    void* restart;
    unsigned restart_delay;
    void* pull;
    void* finished_pull; /* Отсоединяется до внешних callbacks; публикуется на выходе из receive. */
    struct ms_sql_job* sql_job; /* Слабая связь: отмена отсоединяет worker, не освобождая его входы. */
    struct ms_sql_job* draining_controls; /* Завершённый шаг на стеке uasync. Удаление сессии обнуляет его session. */
    struct ms_session** publishing; /* Слабая ссылка RAM-публикации на стеке uasync. */
    int publish_pending; /* Legacy commit: опубликовать ожидание зависимостей на безопасном выходе. */
    int sql_resume; /* Dependency callback пришёл во время публикации SQL. */
    int failure_error;
    struct ms_request* requests;
    uint8_t peer[32], group_key[32];
    uint64_t peer_node, ns_id, round, revision, verified_revision;
    uint8_t round_id[MS_TOKEN_SIZE], accept_id[MS_TOKEN_SIZE];
    char ns[65], tree_ns[70];
    int state, leader, dirty, depth, after_valid, waiting;
    uint32_t request_id, remote_request_id;
    struct merkle_sync_dependency fetch;
    merkle_key_t after; /* Курсор обычного обхода листа. */
    struct ms_frame stack[MT_MAX_LEVEL + 1];
    uint8_t (*source_children)[MT_BUCKETS * MT_HASH_SIZE]; /* Доказательство текущего входящего прохода MEMBERS. */
    uint8_t source_root[MT_HASH_SIZE], source_expected[MT_HASH_SIZE];
    uint8_t local_root[MT_HASH_SIZE], served_root[MT_HASH_SIZE];
    int served_root_known, served_confirmed;
    uint8_t previous_roots[2 * MT_HASH_SIZE];
    int previous_valid;
    uint8_t mode; /* EXCHANGE либо локальный READER/WRITER независимого однонаправленного запроса. */
    uint64_t sent_bytes, received_bytes, registration;
};

struct merkle_sync {
    struct UTUN_INSTANCE* inst;
    uint8_t svc_id;
    struct ms_dataset datasets[256];
    struct ms_session* sessions;
    struct ms_sql_job* cleanups;
    uint64_t round_seq;
    struct ETCP_CONN* closing_conn;
};

static uint64_t ms_read64(const uint8_t* p) { uint64_t n; memcpy(&n, p, 8); return be64toh(n); }
static void ms_write64(uint8_t* p, uint64_t n) { n = htobe64(n); memcpy(p, &n, 8); }
static uint32_t ms_read32(const uint8_t* p) { uint32_t n; memcpy(&n, p, 4); return ntohl(n); }
static void ms_write32(uint8_t* p, uint32_t n) { n = htonl(n); memcpy(p, &n, 4); }

static int ms_token_empty(const uint8_t token[MS_TOKEN_SIZE]) {
    static const uint8_t zero[MS_TOKEN_SIZE];
    return !memcmp(token, zero, sizeof(zero));
}

static int ms_new_token(uint8_t token[MS_TOKEN_SIZE]) {
    if (random_bytes(token, MS_TOKEN_SIZE) == 0 && !ms_token_empty(token)) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: handshake RNG failed"); return MT_ERR_IO;
}

static int ms_namespace(const char* ns, uint64_t* id) {
    if (!ns || !ns[0] || strlen(ns) > 20) return -1;
    for (const char* p = ns; *p; p++) if (*p < '0' || *p > '9') return -1;
    if (ns[0] == '0' && ns[1]) return -1;
    char* end;
    errno = 0;
    unsigned long long n = strtoull(ns, &end, 10);
    if (errno || *end) return -1;
    *id = (uint64_t)n;
    return 0;
}

static struct ms_session* ms_find(struct merkle_sync* ms, struct ETCP_CONN* conn, const struct chat_join_path* join,
    uint8_t dataset, uint64_t ns_id, uint8_t mode) {
    for (struct ms_session* s = ms->sessions; s; s = s->next) {
        if (s->conn == conn && (!join || !memcmp(join, &s->join_path, sizeof(*join))) &&
            s->dataset->id == dataset && s->ns_id == ns_id && s->mode == mode) return s;
    }
    return NULL;
}

/* Нужен именно текущий UP transport: удержание не подключает адреса и не меняет право модели на обмен. */
static int ms_hold_transport(struct ms_session* s) {
    if (s->join_path.attempt || !s->dataset->ops->keep_transport || s->transport) return 0;
    if (node_conn_direct_hold(s->conn, &s->transport) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: transport hold failed dataset=%u ns=%s peer=%s",
            s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0})); return MT_ERR_DISCONNECTED;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: transport retained dataset=%u ns=%s peer=%s",
        s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0})); return 0;
}

static int ms_members(const struct ms_session* s);

static void ms_notify(struct ms_session* s, int result) {
    if (result == MT_OK) s->restart_delay = 0;
    /* Все данные копируются до callback: callback вправе отменить сессию. */
    struct ms_request* r = s->requests;
    s->requests = NULL;
    struct NODE_CONN_DIRECT* transport = NULL;
    if (s->state != MS_FAILED || !s->restart || s->mode != MS_EXCHANGE || !ms_members(s)) {
        transport = s->transport; s->transport = NULL;
    }
    if (transport) DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: releasing transport dataset=%u ns=%s result=%d",
        s->dataset->id, s->ns, result);
    uint8_t peer[32]; memcpy(peer, s->peer, 32);
    uint8_t mode = s->mode;
    char ns[sizeof(s->ns)]; memcpy(ns, s->ns, sizeof(ns));
    void (*on_result)(void*, const char*, const uint8_t*, int) = s->dataset->ops->on_result;
    void* ctx = s->dataset->data_ctx;
    if (on_result && mode != MS_WRITER) on_result(ctx, ns, peer, result);
    while (r) {
        struct ms_request* next = r->next;
        merkle_sync_done_cb cb = r->cb; void* arg = r->arg;
        u_free(r);
        cb(peer, ns, result, arg);
        r = next;
    }
    node_conn_direct_close(transport);
}

static int ms_schedule(struct ms_session* s);
static void ms_work(void* arg);
static void ms_fail(struct ms_session* s, int error, const char* reason, int report);
static void ms_sql_cancel(struct ms_session* s);
static int ms_cleanup_pending(const struct ms_session* s);
static void ms_sql_abort(struct UTUN_INSTANCE* inst, const struct merkle_sync_sql_ops* ops,
    const char* ns, const uint8_t peer[32], uint8_t dataset, uint64_t round, int members, int wait_slot, void* pull);

enum ms_sql_continuation {
    MS_DB_SERVE_HASHES, MS_DB_SERVE_PAGE, MS_DB_COMPARE_HASHES, MS_DB_APPLY_PAGE, MS_DB_BEGIN,
    MS_DB_COMMIT_WALK, MS_DB_COMMIT_READER, MS_DB_FINISH_WALK, MS_DB_SEND_CHECK,
    MS_DB_COMPARE_CHECK, MS_DB_FINISH_DONE, MS_DB_COMPARE_DONE, MS_DB_CONFIRM_SOURCE, MS_DB_ABORT,
    MS_DB_SERVE_DEPENDENCY, MS_DB_APPLY_DEPENDENCY
};
/* Для отложенной проверки сохраняем только поля команды. error — проверенный положительный код MS_ERROR. */
struct ms_control {
    uint64_t round;
    uint8_t round_id[MS_TOKEN_SIZE], accept_id[MS_TOKEN_SIZE];
    uint8_t type, valid, error;
};
struct ms_sql_job {
    struct UTUN_INSTANCE* inst; /* Guard/done только uasync; worker эти поля не использует. */
    struct db_async* backend;
    struct merkle_sync* owner;
    struct ms_session* session;
    const struct merkle_sync_sql_ops* ops;
    struct merkle_sync_sql_request request;
    void* scope;
    char tree_ns[70];
    uint8_t level, hashes[MT_BUCKETS][MT_HASH_SIZE], remote[MT_BUCKETS * MT_HASH_SIZE];
    uint8_t pinned_root[MT_HASH_SIZE], read_root[MT_HASH_SIZE];
    uint64_t generation;
    uint64_t peer_node;
    uint32_t wire_request;
    uint8_t dataset;
    enum ms_sql_continuation continuation;
    struct ms_control controls[MS_CONTROL_LIMIT]; /* FIFO принадлежит сессии до окончания этого шага, включая смену раунда. */
    unsigned control_count; /* LIMIT+1 означает переполнение до публикации. Только uasync. */
    int model, owns_pull, members, check_pinned_root, read_pinned_root;
    struct ms_sql_job* cleanup_next;
    int wait_slot, cleanup_tracked;
    void* retry_timer;
    unsigned retry_delay;
    uint64_t registration;
    int* submission_result; /* Только синхронный отказ run; stack caller жив до возврата run. */
};
static int ms_sql_submit(struct ms_session* s, enum ms_sql_continuation continuation,
    struct merkle_sync_sql_request* request, uint8_t level, uint32_t wire_request, const uint8_t* remote, size_t remote_size);
static void ms_sql_cleanup(void* arg);
static void ms_sql_retry(void* arg);
static int ms_sql_wait(struct ms_sql_job* job, const char* reason);
static int ms_sql_enabled(struct ms_session* s) { return db_async_can_write(s->ms->inst->db_worker); }
static int ms_members(const struct ms_session* s) {
    return (s->dataset->id == MT_DATA_MEMBERS || s->dataset->id == MT_DATA_MEMBER_ADMIN) && s->dataset->ops->group_scoped;
}

static int ms_registration_current(struct UTUN_INSTANCE* inst, const char* ns, uint64_t registration) {
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, ns);
    return group && !group->stopping && group->group_type == TOPO_GROUP_TYPE_CHAT && group->generation == registration;
}

static int ms_cleanup_pending(const struct ms_session* s) {
    for (const struct ms_sql_job* j = s->ms->cleanups; j; j = j->cleanup_next)
        if (j->dataset == s->dataset->id && !strcmp(j->request.ns, s->ns) && !memcmp(j->request.peer, s->peer, 32)) return 1;
    return 0;
}
static void ms_track_cleanup(struct ms_sql_job* job) {
    if (!job->wait_slot || !job->owner || job->cleanup_tracked) return;
    job->cleanup_next = job->owner->cleanups;
    job->owner->cleanups = job; job->cleanup_tracked = 1;
}
static void ms_untrack_cleanup(struct ms_sql_job* job) {
    if (!job->cleanup_tracked) return;
    struct ms_sql_job** link = &job->owner->cleanups;
    while (*link && *link != job) link = &(*link)->cleanup_next;
    if (*link) *link = job->cleanup_next;
    job->cleanup_tracked = 0; job->cleanup_next = NULL;
}

static void ms_abort_pull(struct ms_session* s) {
    u_free(s->source_children); s->source_children = NULL;
    ms_sql_cancel(s); s->publish_pending=0; s->fetch.length = 0;
    if (!s->pull) return;
    void* pull = s->pull; s->pull = NULL;
    if (ms_sql_enabled(s)) {
        int members = ms_members(s);
        ms_sql_abort(s->ms->inst, s->dataset->ops->sql, s->ns, s->peer, s->dataset->id,
            s->round, members, members || s->dataset->ops->dependency != NULL, pull);
    } else s->dataset->ops->abort_pull(s->dataset->data_ctx, pull);
}

/* Commit не вызывает внешний код; публикация откладывается до безопасного выхода из receive. */
static int ms_commit_pull(struct ms_session* s) {
    const struct merkle_sync_data_ops* ops = s->dataset->ops;
    if (ms_members(s) && !ms_registration_current(s->ms->inst, s->ns, s->registration)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle commit registration expired ns=%s registration=%llu",
            s->ns, (unsigned long long)s->registration);
        ms_abort_pull(s);
        return MT_ERR_DISCONNECTED;
    }
    sqlite3* db = s->ms->inst->topo_sqlite_db;
    struct db_changes_origin source = db_changes_source(db, s->peer, s->peer_node, "Merkle sync");
    int rc = ops->commit_pull(s->dataset->data_ctx, s->pull);
    db_changes_restore(db, source);
    if (rc == MT_APPLY_PENDING) { s->publish_pending=1; return rc; }
    if (rc >= 0 && s->dataset->ops->publish_pull) { s->finished_pull = s->pull; s->pull = NULL; }
    else ms_abort_pull(s);
    return rc;
}

static void ms_cancel_deadline(struct ms_session* s) {
    if (s->deadline) uasync_cancel_timeout(s->ms->inst->ua, s->deadline);
    s->deadline = NULL;
}

static void ms_stop_deadline(struct ms_session* s) {
    ms_cancel_deadline(s);
    s->round_deadline_tb = 0;
}

static int ms_round_expired(const struct ms_session* s) {
    return s->round_deadline_tb && get_time_tb() >= s->round_deadline_tb;
}

static void ms_timeout(void* arg) {
    struct ms_session* s = arg;
    s->deadline = NULL;
    ms_fail(s, MT_ERR_TIMEOUT, "no protocol progress for 30 seconds", 0);
}

static int ms_progress(struct ms_session* s) {
    int delay = MS_PROGRESS_TIMEOUT_TB;
    if (s->round_deadline_tb) {
        uint64_t now = get_time_tb();
        uint64_t remaining = now < s->round_deadline_tb ? s->round_deadline_tb - now : 0;
        if (remaining < MS_PROGRESS_TIMEOUT_TB) delay = (int)remaining;
    }
    ms_cancel_deadline(s);
    s->deadline = uasync_set_timeout(s->ms->inst->ua, delay, s, ms_timeout, "merkle_progress");
    if (!s->deadline) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: deadline allocation ns=%s", s->ns); return MT_ERR_IO;
    }
    return 0;
}

static void ms_discard_tx(struct ms_session* s) {
    if (s->join_tx) {
        uasync_call_soon_cancel(s->ms->inst->ua, s->join_tx);
        s->join_tx = NULL;
    }
    if (s->send_q) queue_waiter_cancel(s->send_q, &s->waiter);
    if (s->tx) { queue_dgram_free(s->tx); queue_entry_free(s->tx); s->tx = NULL; }
}
static void ms_cancel_restart(struct ms_session* s) {
    if (s->restart) uasync_cancel_timeout(s->ms->inst->ua, s->restart);
    s->restart = NULL;
}

static void ms_free(struct ms_session* s) {
    struct merkle_sync* ms = s->ms;
    struct ms_session** p = &ms->sessions;
    while (*p && *p != s) p = &(*p)->next;
    if (*p) *p = s->next;
    if (s->work) uasync_call_soon_cancel(ms->inst->ua, s->work);
    ms_cancel_restart(s);
    ms_stop_deadline(s);
    if (s->draining_controls) s->draining_controls->session = NULL;
    if (s->publishing) *s->publishing = NULL;
    ms_abort_pull(s);
    ms_discard_tx(s);
    if (s->finished_pull) s->dataset->ops->abort_pull(s->dataset->data_ctx, s->finished_pull);
    while (s->requests) {
        struct ms_request* r = s->requests;
        s->requests = r->next; u_free(r);
    }
    struct ETCP_CONN* conn = s->conn;
    struct NODE_CONN_DIRECT* transport = s->transport;
    if (transport) DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: releasing cancelled transport dataset=%u ns=%s",
        s->dataset->id, s->ns);
    u_free(s);
    node_conn_direct_close(transport);
    etcp_conn_ref_free(conn);
}

static int ms_local_leads(const struct merkle_sync* ms, const struct ETCP_CONN* conn, uint8_t mode) {
    return mode == MS_READER || (mode == MS_EXCHANGE && memcmp(ms->inst->my_ed25519_pubkey, conn->peer_ed25519_pubkey, 32) < 0);
}

/* Сквозной MEMBERS связывает сессию с конечным пиром JOIN, прямой обмен — с пиром ETCP. */
static const uint8_t* ms_remote_key(const struct UTUN_INSTANCE* inst, const struct ETCP_CONN* conn, const struct chat_join_path* join) {
    if (conn) return conn->peer_ed25519_pubkey;
    if (!join || !inst) return NULL;
    return !memcmp(inst->my_ed25519_pubkey, join->inviter, 32) ? join->guest : join->inviter;
}

static struct ms_session* ms_create(struct merkle_sync* ms, struct ETCP_CONN* conn, const struct chat_join_path* join,
    uint8_t dataset, uint64_t ns_id, uint8_t mode) {
    if (!join && (!conn || !conn->send_input_q || !conn->initialized || !conn->links_up || conn->state == 2)) return NULL;
    if (join) {
        unsigned active = 0;
        struct ms_session* session = ms->sessions;
        while (session) {
            struct ms_session* next = session->next;
            if (session->join_path.attempt && session->mode == MS_WRITER) {
                if (session->state == MS_COMPLETE || session->state == MS_FAILED) ms_free(session);
                else active++;
            }
            session = next;
        }
        if (mode == MS_WRITER && active >= 64) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN MEMBERS source session limit ns=%llu", (unsigned long long)ns_id);
            return NULL;
        }
    }
    uint8_t local_key[32]; const uint8_t* key = local_key;
    uint64_t registration = 0;
    if (ms->datasets[dataset].ops->group_scoped) {
        struct TOPO_GROUP* group = topo_groups_find(ms->inst->topo_groups, ns_id);
        key = group && group->group_type == TOPO_GROUP_TYPE_CHAT ? topo_group_wire_id(group) : NULL;
        if (!key) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: no group key dataset=%u ns=%llu", dataset, (unsigned long long)ns_id);
            return NULL;
        }
        registration = group->generation;
    } else {
        topo_group_utun_wire_id(local_key, ns_id);
    }
    struct ms_session* s = u_calloc(1, sizeof(*s));
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: session allocation failed"); return NULL; }
    if (conn && etcp_conn_ref_take(conn) != 0) { u_free(s); return NULL; }
    s->ms = ms; s->dataset = &ms->datasets[dataset]; s->conn = conn; s->ns_id = ns_id;
    if (join) {
        s->join_path = *join;
        memcpy(s->peer, mode == MS_READER ? join->inviter : join->guest, 32);
        uint8_t x[32];
        if (sc_identity_public_x("JOIN MEMBERS peer", s->peer, x) != SC_OK) { u_free(s); return NULL; }
        s->peer_node = sc_derive_node_id_from_pubkey(x);
    } else {
        s->send_q = conn->send_input_q;
        memcpy(s->peer, conn->peer_ed25519_pubkey, 32);
        s->peer_node = conn->peer_node_id;
    }
    memcpy(s->group_key, key, 32);
    if (ms->datasets[dataset].ops->group_scoped) sc_public_key_hex(key, s->ns);
    else snprintf(s->ns, sizeof(s->ns), "%llu", (unsigned long long)ns_id);
    snprintf(s->tree_ns, sizeof(s->tree_ns), "%u:%s", dataset, s->ns);
    s->mode = mode;
    s->registration = registration;
    s->leader = ms_local_leads(ms, conn, mode);
    if (ms_hold_transport(s) < 0) { etcp_conn_ref_free(conn); u_free(s); return NULL; }
    s->next = ms->sessions; ms->sessions = s;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: attach dataset=%u ns=%s peer=%s conn=%p leader=%d mode=%u",
        s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0}), (void*)conn, s->leader, mode);
    return s;
}

int merkle_sync_read_hash(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns,
        uint8_t level, uint64_t prefix, uint8_t hash[MT_HASH_SIZE]) {
    char tree_ns[70];
    if (!ns || strlen(ns) > 64) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: invalid hash namespace"); return -1; }
    snprintf(tree_ns, sizeof(tree_ns), "%u:%s", dataset, ns);
    return merkle_tree_get(inst ? inst->topo_sqlite_db : NULL, tree_ns, level, prefix, hash);
}

static int ms_root(struct ms_session* s, uint8_t root[MT_HASH_SIZE]) {
    return merkle_sync_read_hash(s->ms->inst, s->dataset->id, s->ns, 0, 0, root);
}

/* Проверяет тот корень, из которого эта SQL-транзакция читает или применяет данные. */
static int ms_check_root(sqlite3* db, const char* tree, const uint8_t* pinned, const char* ns, uint64_t round, int* sql_code) {
    uint8_t current[MT_HASH_SIZE];
    int rc = merkle_tree_get(db, tree, 0, 0, current);
    if (rc < 0) {
        if (sql_code) *sql_code = -rc;
        return MT_ERR_IO;
    }
    if (!memcmp(current, pinned, sizeof(current))) return MT_OK;
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS snapshot changed ns=%s round=%llu pinned=%s current=%s",
        ns, (unsigned long long)round, sc_public_key_hex(pinned, (char[65]){0}), sc_public_key_hex(current, (char[65]){0}));
    return MT_ERR_STALE;
}

static int ms_allowed(struct ms_dataset* model, struct ETCP_CONN* conn, const char* ns, uint8_t mode) {
    if (mode == MS_READER)
        return model->ops->validate_request && model->ops->validate_request(model->data_ctx, ns, conn) == 1;
    return !model->ops->validate_peer || model->ops->validate_peer(model->data_ctx, ns, conn) == 1;
}

static int ms_can_run(struct ms_dataset* model, struct ETCP_CONN* conn, const char* ns, uint8_t mode) {
    if (!ms_allowed(model, conn, ns, mode)) return 0;
    return model->ops->can_run ? model->ops->can_run(model->data_ctx, ns, conn) == 1 : !conn->peer_sleep_phase;
}

static int ms_session_allowed(const struct ms_session* s) {
    if (s->join_path.attempt) return chat_sync_join_members_allowed(s->ms->inst, &s->join_path, s->ns);
    return ms_allowed(s->dataset, s->conn, s->ns, s->mode);
}

static int ms_session_can_run(const struct ms_session* s) {
    if (s->join_path.attempt) return ms_session_allowed(s);
    return ms_can_run(s->dataset, s->conn, s->ns, s->mode);
}

static size_t ms_frame_limit(const struct ms_session* s) {
    return s->join_path.attempt ? CHAT_JOIN_BODY_MAX : MT_PAGE_SIZE;
}

static void ms_restart(void* arg);
static int ms_delay_round(struct ms_session* s) {
    if (s->restart) return MT_OK;
    unsigned delay = s->restart_delay ? s->restart_delay : MS_ROUND_RETRY_MIN_TB;
    s->restart = uasync_set_timeout(s->ms->inst->ua, delay, s, ms_restart, "merkle_round_retry");
    if (!s->restart) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle retry timer allocation ns=%s peer=%s", s->ns, sc_public_key_hex(s->peer, (char[65]){0}));
        return MT_ERR_IO;
    }
    s->restart_delay = delay < MS_ROUND_RETRY_MAX_TB / 2 ? delay * 2 : MS_ROUND_RETRY_MAX_TB;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle new round deferred dataset=%u ns=%s peer=%s round=%llu leader=%d result=%d delay_tb=%u",
        s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round, s->leader, s->failure_error, delay);
    return MT_OK;
}
static void ms_restart(void* arg) {
    struct ms_session* s = arg;
    s->restart = NULL;
    if (s->state != MS_FAILED) return;
    if (!ms_session_can_run(s) ||
        (ms_members(s) && !ms_registration_current(s->ms->inst, s->ns, s->registration))) {
        ms_fail(s, MT_ERR_DISCONNECTED, "round retry no longer allowed", 0); return;
    }
    if (ms_cleanup_pending(s)) {
        if (ms_delay_round(s) < 0) ms_fail(s, MT_ERR_IO, "round cleanup retry scheduling", 0);
        return;
    }
    s->state = MS_IDLE; s->dirty = 1;
    if (ms_schedule(s) < 0) ms_fail(s, MT_ERR_IO, "round retry scheduling", 0);
}
static void ms_failed(struct ms_session* s, int error, int retry_wake_on_timeout) {
    int retry_members = ms_members(s) && s->mode == MS_EXCHANGE &&
        (error == MT_ERR_DB_RETRY || (s->leader && error == MT_ERR_STALE) ||
            (error == MT_ERR_TIMEOUT && (s->leader || retry_wake_on_timeout)));
    int retry_dependency = s->leader && s->dataset->ops->dependency && (error == MT_ERR_STALE || error == MT_ERR_TIMEOUT);
    if ((retry_members || retry_dependency) &&
        ms_session_can_run(s)) {
        s->previous_valid = 0;
        ms_delay_round(s);
    }
    ms_notify(s, error);
}

static void ms_sent(struct ll_queue* q, void* arg) {
    struct ms_session* s = arg;
    struct ll_entry* entry = s->tx;
    s->tx = NULL;
    if (!entry) return;
    if (ms_round_expired(s)) {
        queue_dgram_free(entry); queue_entry_free(entry);
        ms_fail(s, MT_ERR_TIMEOUT, "transport handoff", 0);
        return;
    }
    int type = entry->dgram[MS_TYPE_OFF] & ~(MS_READ_ONLY | MS_RESPONSE);
    uint32_t request = ms_read32(entry->dgram + MS_REQUEST_OFF);
    size_t len = entry->len;
    const char* unavailable = NULL;
    if (s->conn && (!s->conn->links_up || s->conn->state == 2 || s->conn->close_requested || q != s->conn->send_input_q))
        unavailable = "connection unavailable";
    if (!unavailable && !ms_session_can_run(s)) unavailable = "model send deferred";
    if (unavailable) {
        queue_dgram_free(entry); queue_entry_free(entry);
        ms_fail(s, MT_ERR_DISCONNECTED, unavailable, 0);
        return;
    }
    if (type == MS_DONE && s->mode == MS_WRITER && s->revision != s->verified_revision) {
        queue_dgram_free(entry); queue_entry_free(entry);
        ms_fail(s, MT_ERR_CONFLICT, "pull source changed before confirmation", 1); return;
    }
    /* Отложенное уведомление не резервирует место: другой отправитель мог его занять. */
    if (q && (q->count > q->threshold_max_packets || (q->threshold_max_bytes && q->total_bytes > q->threshold_max_bytes))) {
        s->tx = entry;
        if (queue_waiter_wait(q, &s->waiter, ms_sent, s) < 0) ms_fail(s, MT_ERR_IO, "transport wait allocation", 0);
        else DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle queue refilled ns=%s round=%llu packets=%d",
            s->ns, (unsigned long long)s->round, q->count);
        return;
    }
    int rc;
    if (s->join_path.attempt) {
        rc = chat_join_send(s->ms->inst, &s->join_path, CS_MSG_JOIN_MEMBERS, entry->dgram, entry->len);
        queue_dgram_free(entry); queue_entry_free(entry);
    } else {
        rc = etcp_send(s->conn, entry);
        if (rc) { queue_dgram_free(entry); queue_entry_free(entry); }
    }
    if (rc) {
        ms_fail(s, MT_ERR_IO, "transport rejected packet", 0);
        return;
    }
    if (type == MS_BEGIN) s->round_deadline_tb = get_time_tb() + MS_ROUND_TIMEOUT_TB;
    s->sent_bytes += len;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: TX self=%s dataset=%u ns=%s peer=%s round=%llu type=%u request=%u bytes=%zu",
        sc_public_key_hex(s->ms->inst->my_ed25519_pubkey, (char[65]){0}), s->dataset->id, s->ns,
        sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round, type, request, len);
    if (type == MS_ERROR) {
        s->state = MS_FAILED;
        ms_stop_deadline(s);
        ms_failed(s, s->failure_error, 0);
        return;
    }
    if (type == MS_SOURCE_OK) {
        if (s->join_path.attempt && s->mode == MS_WRITER) { ms_free(s); return; }
        s->served_confirmed = 1;
        if (s->mode == MS_WRITER) {
            s->state = MS_COMPLETE;
            ms_stop_deadline(s);
            struct NODE_CONN_DIRECT* transport = s->transport; s->transport = NULL;
            node_conn_direct_close(transport);
        }
        return;
    }
    if (type == MS_DONE) {
        s->state = s->mode == MS_WRITER ? MS_COMPLETE : MS_IDLE;
        ms_stop_deadline(s);
        if (s->mode == MS_WRITER) {
            struct NODE_CONN_DIRECT* transport = s->transport; s->transport = NULL;
            if (transport) DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: releasing served transport dataset=%u ns=%s",
                s->dataset->id, s->ns);
            node_conn_direct_close(transport); return; /* Выдача не подтверждает синхронизацию собственного состава. */
        }
        if (s->revision == s->verified_revision) {
            s->dirty = 0;
            DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: converged dataset=%u ns=%s peer=%s round=%llu tx=%llu rx=%llu",
                s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round,
                (unsigned long long)s->sent_bytes, (unsigned long long)s->received_bytes);
            ms_notify(s, MT_OK);
            return;
        }
    }
    if (s->state == MS_IDLE && s->dirty && ms_schedule(s) < 0) ms_fail(s, MT_ERR_IO, "work allocation", 1);
}

/* Сквозной канал тоже отправляет отложенно, чтобы автомат успел сменить фазу. */
static void ms_join_sent(void* arg) {
    struct ms_session* s = arg;
    s->join_tx = NULL;
    ms_sent(NULL, s);
}

static int ms_send(struct ms_session* s, int type, uint32_t request, const uint8_t* payload, size_t len) {
    if (ms_hold_transport(s) < 0) return -1;
    if (s->tx || len > ms_frame_limit(s) - MS_HEADER) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: invalid TX ns=%s type=%d busy=%d size=%zu", s->ns, type, s->tx != NULL, len);
        return -1;
    }
    struct ll_entry* e = ll_alloc_lldgram((uint16_t)(MS_HEADER + len));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: packet allocation failed ns=%s", s->ns); return -1; }
    e->len = (uint16_t)(MS_HEADER + len);
    e->dgram[0] = s->ms->svc_id;
    memcpy(e->dgram + MS_SCOPE_OFF, s->group_key, 32);
    e->dgram[MS_TYPE_OFF] = (uint8_t)type | (s->mode ? MS_READ_ONLY : 0) | (s->mode == MS_WRITER ? MS_RESPONSE : 0);
    ms_write64(e->dgram + MS_ROUND_OFF, type == MS_WAKE ? 0 : s->round);
    ms_write32(e->dgram + MS_REQUEST_OFF, request);
    e->dgram[MS_DATASET_OFF] = s->dataset->id;
    memset(e->dgram + MS_ROUND_ID_OFF, 0, 2 * MS_TOKEN_SIZE);
    if (type != MS_WAKE) memcpy(e->dgram + MS_ROUND_ID_OFF, s->round_id, MS_TOKEN_SIZE);
    if (type != MS_WAKE && type != MS_BEGIN) memcpy(e->dgram + MS_ACCEPT_ID_OFF, s->accept_id, MS_TOKEN_SIZE);
    if (len) memcpy(e->dgram + MS_HEADER, payload, len);
    s->tx = e;
    /* Transport normalizer configures deferred waiters. Не вызываем callback рекурсивно. */
    if (s->join_path.attempt) {
        s->join_tx = uasync_call_soon(s->ms->inst->ua, s, ms_join_sent);
        if (!s->join_tx) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN MEMBERS send scheduling failed ns=%s", s->ns);
            ms_discard_tx(s); return -1;
        }
    } else if (queue_waiter_wait(s->send_q, &s->waiter, ms_sent, s) < 0) { ms_discard_tx(s); return -1; }
    if (ms_progress(s) < 0) { ms_discard_tx(s); return -1; }
    return 0;
}

static void ms_fail(struct ms_session* s, int error, const char* reason, int report) {
    /* После DONE ведущий уже может быть в IDLE. Ведомый сам восстанавливает контрольную точку. */
    int retry_wake_on_timeout = s->state == MS_WAKE_WAIT || (s->state == MS_DONE_WAIT && ms_sql_enabled(s));
    if (s->state == MS_FAILING && s->failure_error == MT_ERR_LIMIT) {
        error = MT_ERR_LIMIT;
        report = 0;
    } else if (error == MT_ERR_TIMEOUT && ms_round_expired(s)) {
        reason = "round lifetime exceeded (600 seconds)";
        report = 0;
    }
    /* Следующий BEGIN/WAKE заменит сорванный раунд с любой стороны. */
    if (error == MT_ERR_DB_RETRY) report = 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: failed self=%s dataset=%u ns=%s peer=%s round=%llu error=%d reason=%s",
        sc_public_key_hex(s->ms->inst->my_ed25519_pubkey, (char[65]){0}), s->dataset->id, s->ns,
        sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round, error, reason);
    s->state = MS_FAILED; s->dirty = 0; s->waiting = 0; s->failure_error = error;
    ms_cancel_restart(s);
    if (s->work) { uasync_call_soon_cancel(s->ms->inst->ua, s->work); s->work = NULL; }
    ms_discard_tx(s);
    ms_cancel_deadline(s);
    ms_abort_pull(s);
    if (report) {
        s->state = MS_FAILING;
        uint8_t code[4]; ms_write32(code, (uint32_t)(-error));
        if (ms_send(s, MS_ERROR, 0, code, sizeof(code)) == 0) return;
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: could not send error ns=%s", s->ns);
        s->state = MS_FAILED;
    }
    s->round_deadline_tb = 0;
    ms_failed(s, error, retry_wake_on_timeout);
}

static int ms_schedule(struct ms_session* s) {
    if (s->state == MS_FAILED) return 0;
    if (ms_hold_transport(s) < 0) return MT_ERR_DISCONNECTED;
    if (s->work || s->ms->inst->ntp.startup_wait) return 0;
    s->work = uasync_call_soon(s->ms->inst->ua, s, ms_work);
    if (!s->work) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: work allocation ns=%s", s->ns); return MT_ERR_IO;
    }
    return 0;
}

static int ms_retry_root_mismatch(struct ms_session* s, const uint8_t local[MT_HASH_SIZE], const uint8_t remote[MT_HASH_SIZE]) {
    if (s->previous_valid && !memcmp(s->previous_roots, local, MT_HASH_SIZE) &&
        !memcmp(s->previous_roots + MT_HASH_SIZE, remote, MT_HASH_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle roots still differ dataset=%u ns=%s peer=%s local=%s remote=%s",
            s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0}),
            sc_public_key_hex(local, (char[65]){0}), sc_public_key_hex(remote, (char[65]){0}));
        return MT_ERR_CONFLICT;
    }
    memcpy(s->previous_roots, local, MT_HASH_SIZE);
    memcpy(s->previous_roots + MT_HASH_SIZE, remote, MT_HASH_SIZE);
    s->previous_valid = 1; s->state = MS_IDLE; s->dirty = 1;
    return ms_schedule(s);
}

static void ms_query_position(const struct ms_session* s, uint8_t* level, uint64_t* prefix, int* valid, merkle_key_t* after) {
    const struct ms_frame* f = &s->stack[s->depth];
    *level = f->level; *prefix = f->prefix; *valid = s->after_valid; *after = s->after;
}

static int ms_query(struct ms_session* s) {
    uint8_t body[MS_QUERY_SIZE], level; uint64_t prefix; int valid; merkle_key_t after;
    ms_query_position(s, &level, &prefix, &valid, &after);
    body[0] = level; ms_write64(body + 1, prefix);
    body[9] = (uint8_t)valid; memcpy(body + 10, after.bytes, MT_KEY_SIZE);
    if (++s->request_id == 0) return -1;
    s->waiting = 1;
    return ms_send(s, MS_QUERY, s->request_id, body, sizeof(body));
}

static int ms_query_dependency(struct ms_session* s) {
    if (s->mode != MS_EXCHANGE || s->state != MS_APPLY_WAIT || s->waiting || !s->pull ||
        s->dataset->ops->dependency(s->pull, &s->fetch) < 0 || !s->fetch.length || s->fetch.length > MT_DEPENDENCY_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "merkle missing dependency ns=%s round=%llu state=%d mode=%u waiting=%d bytes=%zu",
            s->ns, (unsigned long long)s->round, s->state, s->mode, s->waiting, s->fetch.length);
        return MT_ERR_DATA;
    }
    if (++s->request_id == 0) return MT_ERR_PROTOCOL;
    s->waiting = 1;
    return ms_send(s, MS_DEP_QUERY, s->request_id, s->fetch.data, s->fetch.length) < 0 ? MT_ERR_IO : MT_OK;
}

static int ms_pull_start(struct ms_session* s) {
    ms_abort_pull(s);
    if (ms_members(s)) {
        s->source_children = u_calloc(MT_MAX_LEVEL, sizeof(*s->source_children));
        if (!s->source_children) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle pull proof allocation ns=%s", s->ns); return MT_ERR_IO;
        }
        if (!ms_sql_enabled(s) && ms_root(s, s->local_root) < 0) return MT_ERR_IO;
    }
    if (ms_sql_enabled(s) && s->dataset->ops->begin_pull) {
        struct merkle_sync_sql_request request = {.action=MT_SQL_BEGIN_PULL};
        return ms_sql_submit(s, MS_DB_BEGIN, &request, 0, 0, NULL, 0);
    }
    if (s->dataset->ops->begin_pull) {
        int rc = s->dataset->ops->begin_pull(s->dataset->data_ctx, s->ns, s->peer, s->round, &s->pull);
        if (rc < 0 || !s->pull) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle begin pull failed dataset=%u ns=%s rc=%d", s->dataset->id, s->ns, rc);
            return rc < 0 ? rc : MT_ERR_IO;
        }
    }
    s->state = MS_PULL; s->depth = 0; s->after_valid = 0; memset(&s->after, 0, sizeof(s->after));
    memset(s->stack, 0, sizeof(s->stack));
    return ms_query(s);
}

static int ms_walk_next(struct ms_session* s) {
    while (s->depth >= 0) {
        struct ms_frame* f = &s->stack[s->depth];
        if (f->children) {
            unsigned i = (unsigned)__builtin_ctz(f->children);
            f->children &= ~(UINT32_C(1) << i);
            uint8_t level = f->level + 1;
            uint64_t prefix = f->prefix | ((uint64_t)i << (64 - level * 5));
            if (s->source_children) memcpy(s->source_expected, s->source_children[f->level] + i * MT_HASH_SIZE, MT_HASH_SIZE);
            s->stack[++s->depth] = (struct ms_frame){ .prefix = prefix, .level = level };
            s->after_valid = 0; memset(&s->after, 0, sizeof(s->after));
            return ms_query(s);
        }
        s->depth--;
    }
    if (s->source_children) {
        u_free(s->source_children); s->source_children = NULL;
        s->state = MS_SOURCE_WAIT;
        return ms_send(s, MS_SOURCE_CHECK, 0, s->source_root, MT_HASH_SIZE);
    }
    if (s->mode == MS_READER) {
        s->state = MS_DONE_WAIT;
        return ms_send(s, MS_CHECK, 0, NULL, 0);
    }
    if (s->pull) {
        if (ms_sql_enabled(s)) {
            struct merkle_sync_sql_request request = {.action=MT_SQL_COMMIT_PULL, .pull=s->pull};
            return ms_sql_submit(s, MS_DB_COMMIT_WALK, &request, 0, 0, NULL, 0);
        }
        int rc = ms_commit_pull(s);
        if (rc < 0) return rc;
        if (rc == MT_APPLY_PENDING) {
            s->state = MS_APPLY_WAIT;
            if (s->dataset->ops->dependency) return ms_query_dependency(s);
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: awaiting model dependencies dataset=%u ns=%s peer=%s",
                s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0}));
            return ms_progress(s);
        }
        if (ms_members(s)) { s->state = MS_MEMBERS_COMMITTED; return MT_OK; }
    }
    if (s->leader) { s->state = MS_SERVE; return ms_send(s, MS_TURN, 0, NULL, 0); }
    if (ms_sql_enabled(s)) {
        struct merkle_sync_sql_request request = {.action=MT_SQL_FINISH};
        return ms_sql_submit(s, MS_DB_FINISH_WALK, &request, 0, 0, NULL, 0);
    }
    if (s->dataset->ops->finish && s->dataset->ops->finish(s->dataset->data_ctx, s->ns) < 0) return MT_ERR_DATA;
    uint8_t root[MT_HASH_SIZE];
    if (ms_root(s, root) < 0) return -1;
    s->state = MS_DONE_WAIT;
    return ms_send(s, MS_CHECK, 0, root, sizeof(root));
}

static int ms_begin(struct ms_session* s) {
    ms_stop_deadline(s);
    ms_abort_pull(s);
    if (ms_cleanup_pending(s)) {
        s->state = MS_FAILED; s->waiting = 0;
        return ms_delay_round(s);
    }
    ms_cancel_restart(s);
    uint8_t token[MS_TOKEN_SIZE];
    if (ms_new_token(token) < 0) return MT_ERR_IO;
    s->round = ++s->ms->round_seq;
    if (!s->round) return -1;
    memcpy(s->round_id, token, sizeof(token));
    memset(s->accept_id, 0, sizeof(s->accept_id));
    s->request_id = s->remote_request_id = 0;
    s->state = MS_BEGIN_WAIT; s->dirty = 0; s->waiting = 0;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: begin self=%s dataset=%u ns=%s peer=%s round=%llu",
        sc_public_key_hex(s->ms->inst->my_ed25519_pubkey, (char[65]){0}), s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round);
    return ms_send(s, MS_BEGIN, 0, NULL, 0);
}

static void ms_work(void* arg) {
    struct ms_session* s = arg;
    s->work = NULL;
    if (ms_round_expired(s)) { ms_fail(s, MT_ERR_TIMEOUT, "scheduled work", 0); return; }
    if (s->ms->inst->ntp.startup_wait) return;
    if (s->sql_job) { s->sql_resume = 1; return; }
    if (s->state == MS_APPLY_WAIT || s->state == MS_READER_APPLY_WAIT) {
        int rc, notify = 0;
        if (!ms_session_can_run(s)) {
            ms_fail(s, MT_ERR_DISCONNECTED, "dependencies ready after peer permission changed", 0); return;
        }
        if (s->state == MS_APPLY_WAIT) { s->state = MS_PULL; rc = ms_walk_next(s); }
        else {
            if (ms_sql_enabled(s)) {
                struct merkle_sync_sql_request request = {.action=MT_SQL_COMMIT_PULL, .pull=s->pull};
                rc = ms_sql_submit(s, MS_DB_COMMIT_READER, &request, 0, 0, NULL, 0);
                if (rc < 0) ms_fail(s, rc, "resume database application", 1);
                return;
            }
            rc = ms_commit_pull(s);
            if (ms_round_expired(s)) rc = MT_ERR_TIMEOUT;
            else if (rc == MT_APPLY_PENDING) rc = ms_progress(s);
            else if (rc >= 0) {
                s->state = MS_COMPLETE; s->dirty = 0; ms_stop_deadline(s); notify = 1;
            }
        }
        void* pending=s->publish_pending?s->pull:NULL; s->publish_pending=0;
        void* finished = s->finished_pull; s->finished_pull = NULL;
        const struct merkle_sync_data_ops* ops = s->dataset->ops; void* context = s->dataset->data_ctx;
        if (rc < 0) ms_fail(s, rc, "resume model application", 1);
        else if (notify) ms_notify(s, MT_OK);
        if(pending && rc>=0 && ops->publish_pull) ops->publish_pull(context,pending);
        if (finished) { ops->publish_pull(context, finished); ops->abort_pull(context, finished); }
        return;
    }
    if (s->state != MS_IDLE || !s->dirty || s->tx) return;
    if (!ms_session_can_run(s)) {
        ms_fail(s, MT_ERR_DISCONNECTED, "model work deferred", 0); return;
    }
    int rc;
    if (s->leader) rc = ms_begin(s);
    else {
        s->dirty = 0;
        if (ms_members(s)) s->state = MS_WAKE_WAIT;
        rc = ms_send(s, MS_WAKE, 0, NULL, 0);
    }
    if (rc < 0) ms_fail(s, MT_ERR_IO, "start send", 1);
}

void merkle_sync_resume_pull(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms || !ns) return;
    for (struct ms_session* s = ms->sessions; s; s = s->next) {
        if (s->dataset->id != dataset || strcmp(s->ns, ns) ||
            s->dataset->ops->dependency || (s->state != MS_APPLY_WAIT && s->state != MS_READER_APPLY_WAIT)) continue;
        if (ms_schedule(s) < 0) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC,
            "merkle_sync: cannot resume model application dataset=%u ns=%s", dataset, ns);
    }
}

/* Удержанные start/changed сохраняют dirty и callbacks; после NTP выполняются обычным автоматом. */
void merkle_sync_time_ready(struct UTUN_INSTANCE* inst) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms || inst->ntp.startup_wait) return;
    for (struct ms_session* s = ms->sessions; s; s = s->next) {
        if (s->state == MS_IDLE && s->dirty && ms_schedule(s) < 0)
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: cannot resume after initial time ns=%s", s->ns);
    }
}

void merkle_sync_changed(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms || !ns) return;
    for (struct ms_session* s = ms->sessions; s; s = s->next) {
        if (s->dataset->id != dataset || strcmp(s->ns, ns)) continue;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: changed self=%s dataset=%u ns=%s peer=%s mode=%u state=%d tx=%d dirty=%u",
            sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), dataset, ns, sc_public_key_hex(s->peer, (char[65]){0}), s->mode, s->state, s->tx != NULL, s->dirty);
        s->revision++;
        if (s->mode != MS_EXCHANGE) continue; /* Pull — один запрос; изменение источника проверяется перед DONE. */
        s->dirty = 1;
        if (s->restart) continue;
        if (s->state == MS_FAILED) { s->state = MS_IDLE; s->previous_valid = 0; }
        if (s->state == MS_IDLE && ms_session_can_run(s) && ms_schedule(s) < 0) {
            ms_fail(s, MT_ERR_IO, "change scheduling", 1); return;
        }
    }
}

/* Сессия остаётся подписанной, но не ждёт пакет, отбрасываемый из-за отзыва допуска. */
void merkle_sync_revalidate_peers(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns) {
    if (!ns || strlen(ns) > 64) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: invalid permission namespace"); return;
    }
    char owned_ns[65]; strcpy(owned_ns, ns);
    for (;;) {
        struct merkle_sync* ms = inst ? inst->msync : NULL;
        if (!ms || !ms->datasets[dataset].ops || !ms->datasets[dataset].ops->validate_peer) return;
        struct ms_session* selected = NULL;
        for (struct ms_session* s = ms->sessions; s; s = s->next) {
            if (s->dataset->id == dataset && !strcmp(s->ns, owned_ns) && (s->state != MS_FAILED || s->restart) && s->state != MS_COMPLETE &&
                !ms_session_allowed(s)) { selected = s; break; }
        }
        if (!selected) return;
        ms_fail(selected, MT_ERR_DATA, "model permission revoked", 0);
    }
}

/* Дерево индексируется по dataset:channel, модель получает исходный channel. */
struct ms_leaf_context { struct ms_dataset* dataset; const char* ns; };
static int ms_leaf_hash(void* ctx, const char* tree_ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    struct ms_leaf_context* leaf = ctx;
    (void)tree_ns;
    return leaf->dataset->ops->update_bucket_hash(leaf->dataset->data_ctx, leaf->ns, level, prefix, hash);
}

int merkle_sync_recompute_path(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns, uint64_t key) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "legacy Merkle reindex rejected in RW dataset=%u; use model SQL helper", dataset); return -1;
    }
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!ms || !db || !ns || strlen(ns) > 64 || !ms->datasets[dataset].ops) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: invalid recompute dataset=%u", dataset); return -1;
    }
    char tree_ns[70]; snprintf(tree_ns, sizeof(tree_ns), "%u:%s", dataset, ns);
    struct ms_leaf_context leaf = { &ms->datasets[dataset], ns };
    int own = sqlite3_get_autocommit(db);
    if (own && sqlite3_exec(db, "SAVEPOINT merkle_update", NULL, NULL, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: begin update ns=%s: %s", ns, sqlite3_errmsg(db)); return -1;
    }
    int rc = merkle_tree_update(db, tree_ns, key, ms_leaf_hash, &leaf);
    if (own) {
        if (rc < 0 && sqlite3_exec(db, "ROLLBACK TO merkle_update", NULL, NULL, NULL) != SQLITE_OK)
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: rollback ns=%s: %s", ns, sqlite3_errmsg(db));
        if (sqlite3_exec(db, "RELEASE merkle_update", NULL, NULL, NULL) != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: commit ns=%s: %s", ns, sqlite3_errmsg(db));
            if (sqlite3_exec(db, "ROLLBACK TO merkle_update; RELEASE merkle_update", NULL, NULL, NULL) != SQLITE_OK)
                DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: commit cleanup ns=%s: %s", ns, sqlite3_errmsg(db));
            rc = -1;
        }
        if (rc > 0) merkle_sync_changed(inst, dataset, ns);
    }
    if (rc < 0) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: recompute failed ns=%s key=%016llx", ns, (unsigned long long)key);
    return rc;
}

/* Первые 8 байтов выбирают ветвь. Модель задаёт длину обязательного нулевого хвоста при регистрации. */
static int ms_cursor_shape(const struct ms_session* s, const merkle_key_t* key) {
    size_t tail = s->dataset->ops->cursor_zero_tail;
    static const merkle_key_t zero;
    return !memcmp(key->bytes + MT_KEY_SIZE - tail, zero.bytes, tail);
}

static const char* ms_page_cursor_error(const struct ms_session* s, uint64_t prefix, const merkle_key_t* after,
    const merkle_key_t* next, int more, size_t size) {
    if (!ms_cursor_shape(s, next)) return "nonzero next suffix";
    if (!more) return NULL;
    if (more != 1) return "invalid more flag";
    if (!size) return "empty continuation page";
    if (merkle_sync_level_prefix(ms_read64(next->bytes), MT_MAX_LEVEL) != prefix) return "next outside requested leaf";
    if (after && memcmp(next->bytes, after->bytes, MT_KEY_SIZE) <= 0) return "next did not advance";
    return NULL;
}

/* Неверный результат get_page — отказ локальной модели, независимо от режима хранения. */
static int ms_page_output(const struct ms_session* s, uint64_t prefix, const merkle_key_t* after,
    const merkle_key_t* next, int more, size_t size, size_t capacity) {
    const char* error = size > capacity ? "capacity exceeded" : ms_page_cursor_error(s, prefix, after, next, more, size);
    if (!error) return MT_OK;
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle local page invalid dataset=%u ns=%s prefix=%016llx more=%d size=%zu capacity=%zu: %s",
        s->dataset->id, s->ns, (unsigned long long)prefix, more, size, capacity, error);
    return MT_ERR_IO;
}

static int ms_serve_query(struct ms_session* s, uint32_t request, const uint8_t* p, size_t len) {
    if (s->mode == MS_READER || s->state != MS_SERVE || len != MS_QUERY_SIZE || request != s->remote_request_id + 1)
        return MT_ERR_PROTOCOL;
    uint8_t level = p[0];
    uint64_t prefix = ms_read64(p + 1);
    merkle_key_t after; memcpy(after.bytes, p + 10, MT_KEY_SIZE);
    static const merkle_key_t empty;
    if (level > MT_MAX_LEVEL || prefix != merkle_sync_level_prefix(prefix, level) || p[9] > 1) return MT_ERR_PROTOCOL;
    if ((!p[9] || level < MT_MAX_LEVEL) && memcmp(&after, &empty, sizeof(after))) return MT_ERR_PROTOCOL;
    if (level < MT_MAX_LEVEL && p[9]) return MT_ERR_PROTOCOL;
    if (p[9]) {
        int shape = ms_cursor_shape(s, &after);
        if (!shape || merkle_sync_level_prefix(ms_read64(after.bytes), MT_MAX_LEVEL) != prefix) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle invalid QUERY cursor dataset=%u ns=%s prefix=%016llx: %s",
                s->dataset->id, s->ns, (unsigned long long)prefix, shape ? "after outside requested leaf" : "nonzero after suffix");
            return MT_ERR_PROTOCOL;
        }
    }
    if (ms_members(s) && !s->served_root_known && (level || request != 1)) return MT_ERR_PROTOCOL;
    s->remote_request_id = request;
    if (ms_members(s) && !ms_sql_enabled(s)) {
        if (!s->served_root_known) {
            if (ms_root(s, s->served_root) < 0) return MT_ERR_IO;
            s->served_root_known = 1;
        } else {
            int rc = ms_check_root(s->ms->inst->topo_sqlite_db, s->tree_ns, s->served_root, s->ns, s->round, NULL);
            if (rc < 0) return rc;
        }
    }
    if (level < MT_MAX_LEVEL) {
        if (ms_sql_enabled(s)) {
            struct merkle_sync_sql_request query = {.prefix=prefix};
            return ms_sql_submit(s, MS_DB_SERVE_HASHES, &query, level, request, NULL, 0);
        }
        uint8_t hashes[MT_BUCKETS][MT_HASH_SIZE];
        if (merkle_tree_children(s->ms->inst->topo_sqlite_db, s->tree_ns, level, prefix, hashes) < 0) return MT_ERR_IO;
        return ms_send(s, MS_HASHES, request, &hashes[0][0], sizeof(hashes)) < 0 ? MT_ERR_IO : 0;
    }
    uint8_t body[MT_PAGE_SIZE - MS_HEADER];
    size_t size = ms_frame_limit(s) - MS_HEADER - MS_PAGE_HEADER;
    if (ms_sql_enabled(s)) {
        struct merkle_sync_sql_request query = {.action=MT_SQL_PAGE, .prefix=prefix, .after_valid=p[9],
            .after=after, .capacity=size};
        return ms_sql_submit(s, MS_DB_SERVE_PAGE, &query, level, request, NULL, 0);
    }
    merkle_key_t next = {{0}}; int more = 0;
    int rc = s->dataset->ops->get_page(s->dataset->data_ctx, s->ns, prefix, p[9], &after,
        body + MS_PAGE_HEADER, &size, &next, &more);
    if (rc < 0) return MT_ERR_IO;
    rc = ms_page_output(s, prefix, p[9] ? &after : NULL, &next, more, size, ms_frame_limit(s) - MS_HEADER - MS_PAGE_HEADER);
    if (rc) return rc;
    body[0] = (uint8_t)more; memcpy(body + 1, next.bytes, MT_KEY_SIZE);
    return ms_send(s, MS_PAGE, request, body, size + MS_PAGE_HEADER) < 0 ? MT_ERR_IO : 0;
}

static int ms_compare_hashes(struct ms_session* s, const uint8_t* local, const uint8_t* remote) {
    struct ms_frame* frame = &s->stack[s->depth];
    if (s->source_children) {
        uint8_t hash[MT_HASH_SIZE];
        if (!merkle_tree_branch_hash(frame->level, remote, hash)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle source branch digest failed ns=%s level=%u", s->ns, frame->level);
            return MT_ERR_IO;
        }
        if (!frame->level) memcpy(s->source_root, hash, MT_HASH_SIZE);
        else if (memcmp(hash, s->source_expected, MT_HASH_SIZE)) {
            struct db_changes_origin source = {.node=s->peer_node, .via="Merkle sync"};
            memcpy(source.ed, s->peer, 32);
            db_changes_warn_hash(NULL, &source, s->ns, "Members Merkle branch", "branch hash mismatch",
                NULL, NULL, s->source_expected, hash);
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle source branch mismatch ns=%s level=%u prefix=%016llx expected=%s got=%s",
                s->ns, frame->level, (unsigned long long)frame->prefix, sc_public_key_hex(s->source_expected, (char[65]){0}),
                sc_public_key_hex(hash, (char[65]){0}));
            return MT_ERR_CONFLICT;
        }
        memcpy(s->source_children[frame->level], remote, MT_BUCKETS * MT_HASH_SIZE);
    }
    frame->children = 0;
    for (unsigned i = 0; i < MT_BUCKETS; i++)
        if (memcmp(local + i * MT_HASH_SIZE, remote + i * MT_HASH_SIZE, MT_HASH_SIZE)) frame->children |= UINT32_C(1) << i;
    s->waiting = 0;
    return ms_walk_next(s);
}

static int ms_receive_hashes(struct ms_session* s, uint32_t request, const uint8_t* p, size_t len) {
    if (s->state != MS_PULL || !s->waiting || request != s->request_id || len != MT_BUCKETS * MT_HASH_SIZE) return MT_ERR_PROTOCOL;
    struct ms_frame* f = &s->stack[s->depth];
    if (f->level >= MT_MAX_LEVEL) return MT_ERR_PROTOCOL;
    if (ms_sql_enabled(s)) return ms_sql_submit(s, MS_DB_COMPARE_HASHES, NULL, f->level, request, p, len);
    uint8_t local[MT_BUCKETS][MT_HASH_SIZE];
    if (ms_members(s)) {
        int rc = ms_check_root(s->ms->inst->topo_sqlite_db, s->tree_ns, s->local_root, s->ns, s->round, NULL);
        if (rc < 0) return rc;
    }
    if (merkle_tree_children(s->ms->inst->topo_sqlite_db, s->tree_ns, f->level, f->prefix, local) < 0) return MT_ERR_IO;
    return ms_compare_hashes(s, &local[0][0], p);
}

static int ms_page_applied(struct ms_session* s, const struct merkle_sync_sql_request* page) {
    s->waiting = 0;
    if (!page->more) return ms_walk_next(s);
    s->after = page->next; s->after_valid = 1;
    return ms_query(s);
}

static int ms_receive_page(struct ms_session* s, uint32_t request, const uint8_t* p, size_t len) {
    if (s->state != MS_PULL || !s->waiting || request != s->request_id || len < MS_PAGE_HEADER) return MT_ERR_PROTOCOL;
    uint8_t level; uint64_t prefix; int valid; merkle_key_t after;
    ms_query_position(s, &level, &prefix, &valid, &after);
    merkle_key_t next; memcpy(next.bytes, p + 1, MT_KEY_SIZE);
    if (level != MT_MAX_LEVEL) return MT_ERR_PROTOCOL;
    const char* error = ms_page_cursor_error(s, prefix, valid ? &after : NULL, &next, p[0], len - MS_PAGE_HEADER);
    if (error) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle invalid PAGE cursor dataset=%u ns=%s prefix=%016llx: %s",
            s->dataset->id, s->ns, (unsigned long long)prefix, error);
        return MT_ERR_PROTOCOL;
    }
    const struct merkle_sync_data_ops* ops = s->dataset->ops;
    if (ms_sql_enabled(s) || ops->stage_page_request) {
        struct merkle_sync_sql_request query = {.action=s->pull ? MT_SQL_STAGE_PAGE : MT_SQL_APPLY_PAGE, .pull=s->pull,
            .prefix=prefix, .after_valid=valid, .after=after, .next=next, .more=p[0], .length=len - MS_PAGE_HEADER,
            .verify_source=s->source_children != NULL};
        if (query.verify_source) memcpy(query.source_hash, s->source_expected, MT_HASH_SIZE);
        memcpy(query.data, p + MS_PAGE_HEADER, query.length);
        if (ms_sql_enabled(s)) return ms_sql_submit(s, MS_DB_APPLY_PAGE, &query, level, request, NULL, 0);
        memcpy(query.peer, s->peer, 32);
        sqlite3* db = s->ms->inst->topo_sqlite_db;
        struct db_changes_origin source = db_changes_source(db, s->peer, s->peer_node, "Merkle page");
        int result = ops->stage_page_request(s->dataset->data_ctx, &query);
        db_changes_restore(db, source);
        return result < 0 ? result : ms_page_applied(s, &query);
    }
    sqlite3* db = s->ms->inst->topo_sqlite_db;
    struct db_changes_origin source = db_changes_source(db, s->peer, s->peer_node, "Merkle page");
    int rc;
    if (s->pull) rc = ops->stage_page(s->dataset->data_ctx, s->pull, prefix, valid, &after,
        &next, p[0], p + MS_PAGE_HEADER, len - MS_PAGE_HEADER);
    else if (ops->apply_page) rc = ops->apply_page(s->dataset->data_ctx, s->ns, s->peer, prefix, valid,
        &after, &next, p[0], p + MS_PAGE_HEADER, len - MS_PAGE_HEADER);
    else rc = ops->apply_items(s->dataset->data_ctx, s->ns, s->peer, p + MS_PAGE_HEADER, len - MS_PAGE_HEADER);
    db_changes_restore(db, source);
    if (rc < 0) return s->pull || rc == MT_ERR_CONFLICT ? rc : MT_ERR_DATA;
    s->waiting = 0;
    if (p[0]) { s->after = next; s->after_valid = 1; return ms_query(s); }
    return ms_walk_next(s);
}

static int ms_dependency_result(int code, const struct merkle_sync_sql_request* request) {
    if (code && request->result >= 0) return MT_ERR_IO;
    return request->result;
}

static int ms_serve_dependency(struct ms_session* s, uint32_t request, const uint8_t* p, size_t len) {
    const struct merkle_sync_data_ops* ops = s->dataset->ops;
    if (!ops->dependency || s->mode != MS_EXCHANGE || s->state != MS_SERVE ||
        !len || len > MT_DEPENDENCY_MAX || !request || request != s->remote_request_id + 1) return MT_ERR_PROTOCOL;
    struct merkle_sync_sql_request query = {.action=MT_SQL_SERVE_DEPENDENCY, .capacity=MT_PAGE_SIZE - MS_HEADER};
    query.fetch.length = len;
    memcpy(query.fetch.data, p, len);
    s->remote_request_id = request;
    if (ms_sql_enabled(s)) return ms_sql_submit(s, MS_DB_SERVE_DEPENDENCY, &query, 0, request, NULL, 0);
    strcpy(query.ns, s->ns);
    int code = ops->dependency_work(s->dataset->data_ctx, &query);
    int rc = ms_dependency_result(code, &query);
    if (rc) return rc;
    if (!query.length || query.length > MT_PAGE_SIZE - MS_HEADER) return MT_ERR_DATA;
    return ms_send(s, MS_DEP_REPLY, request, query.data, query.length) < 0 ? MT_ERR_IO : MT_OK;
}

static int ms_dependency_applied(struct ms_session* s) {
    s->fetch.length = 0; s->waiting = 0; s->state = MS_PULL;
    return ms_walk_next(s);
}

static int ms_receive_dependency(struct ms_session** live, uint32_t request, const uint8_t* p, size_t len) {
    struct ms_session* s = *live;
    const struct merkle_sync_data_ops* ops = s->dataset->ops;
    if (!ops->dependency || s->mode != MS_EXCHANGE || !request || !len || len > MT_PAGE_SIZE - MS_HEADER) return MT_ERR_PROTOCOL;
    if (request < s->request_id || (request == s->request_id && !s->fetch.length)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle stale dependency reply ns=%s round=%llu request=%u current=%u",
            s->ns, (unsigned long long)s->round, request, s->request_id);
        return MT_OK;
    }
    if (s->state != MS_APPLY_WAIT || !s->waiting || !s->fetch.length || request != s->request_id) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "merkle unexpected dependency reply ns=%s round=%llu request=%u expected=%u state=%d",
            s->ns, (unsigned long long)s->round, request, s->request_id, s->state);
        return MT_ERR_PROTOCOL;
    }
    struct merkle_sync_sql_request reply = {.action=MT_SQL_APPLY_DEPENDENCY, .pull=s->pull, .fetch=s->fetch, .length=len};
    memcpy(reply.data, p, len);
    if (ms_sql_enabled(s)) return ms_sql_submit(s, MS_DB_APPLY_DEPENDENCY, &reply, 0, request, NULL, 0);
    strcpy(reply.ns, s->ns);
    memcpy(reply.peer, s->peer, 32);
    sqlite3* db = s->ms->inst->topo_sqlite_db;
    struct db_changes_origin source = db_changes_source(db, s->peer, s->peer_node, "Merkle dependency");
    int code = ops->dependency_work(s->dataset->data_ctx, &reply);
    db_changes_restore(db, source);
    int rc = ms_dependency_result(code, &reply);
    if (rc) return rc;
    if (ops->dependency_committed) {
        s->publishing = live;
        ops->dependency_committed(s->dataset->data_ctx, &reply);
        if (!*live) return MT_OK;
        s->publishing = NULL;
    }
    if (ms_round_expired(s)) return MT_ERR_TIMEOUT;
    return ms_dependency_applied(s);
}

static void ms_sql_cancel(struct ms_session* s) {
    struct ms_sql_job* job = s->sql_job;
    if (!job) return;
    job->session = NULL; s->sql_job = NULL; s->sql_resume = 0;
    if (job->retry_timer) {
        uasync_cancel_timeout(job->inst->ua, job->retry_timer); job->retry_timer = NULL;
        ms_sql_cleanup(job);
        return;
    }
    /* Для BEGIN request.pull — выход работающего SQL-потока. Его читает только done. */
    if (job->continuation != MS_DB_BEGIN && s->pull && job->request.pull == s->pull) { s->pull = NULL; job->owns_pull = 1; }
    if (job->continuation == MS_DB_BEGIN || job->owns_pull) ms_track_cleanup(job);
}

static int ms_sql_current(struct ms_sql_job* job) {
    return job->owner && !job->inst->destroying && job->inst->group_started &&
        job->inst->group_generation == job->generation && job->inst->msync == job->owner &&
        (!job->members || ms_registration_current(job->inst, job->request.ns, job->registration));
}

static int ms_sql_guard(void* arg) {
    struct ms_sql_job* job = arg; struct ms_session* s = job->session;
    if (!s || !ms_sql_current(job) || s->sql_job != job || s->round != job->request.round ||
        s->state == MS_FAILED || s->state == MS_FAILING || !ms_session_can_run(s)) return 0;
    if (ms_round_expired(s)) { job->request.result = MT_ERR_TIMEOUT; return 0; }
    if (job->model && job->ops->snapshot && job->ops->snapshot(s->dataset->data_ctx, s->ns, &job->scope)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL snapshot failed dataset=%u ns=%s action=%d",
            s->dataset->id, s->ns, job->request.action); return 0;
    }
    /* Guard выполняется после публикации предыдущих WRITE, перед чтением сравниваемого корня. */
    if (job->continuation == MS_DB_COMPARE_CHECK || job->continuation == MS_DB_COMPARE_DONE) s->verified_revision = s->revision;
    return 1;
}

static int ms_sql_work(sqlite3* db, void* arg) {
    struct ms_sql_job* job = arg; int rc;
    if (job->check_pinned_root) {
        int code = SQLITE_OK;
        rc = ms_check_root(db, job->tree_ns, job->pinned_root, job->request.ns, job->request.round, &code);
        if (rc < 0) { job->request.result = rc; return rc == MT_ERR_STALE ? SQLITE_ABORT : code; }
    }
    if (job->read_pinned_root) {
        rc = merkle_tree_get(db, job->tree_ns, 0, 0, job->read_root);
        if (rc < 0) return -rc;
    }
    if (job->model) {
        struct db_changes_origin source = db_changes_source(db, job->request.peer, job->peer_node, "Merkle sync");
        rc = job->ops->work(db, job->scope, &job->request);
        db_changes_restore(db, source);
        if (!rc && job->request.result < 0) rc = SQLITE_CONSTRAINT;
    } else {
        rc = job->continuation == MS_DB_SERVE_HASHES || job->continuation == MS_DB_COMPARE_HASHES ?
            merkle_tree_children(db, job->tree_ns, job->level, job->request.prefix, job->hashes) :
            merkle_tree_get(db, job->tree_ns, 0, 0, job->hashes[0]);
        if (rc < 0) rc = -rc;
    }
    return rc;
}

static void ms_sql_cleanup(void* arg) {
    struct ms_sql_job* job = arg;
    if (job->retry_timer) return; /* До повторной подачи работой владеет таймер. */
    if (job->scope && job->ops && job->ops->free) job->ops->free(job->scope);
    if (job->owns_pull && job->request.pull) {
        if (job->continuation == MS_DB_ABORT) job->ops->pull_free(job->request.pull);
        else ms_sql_abort(job->inst, job->ops, job->request.ns, job->request.peer, job->dataset,
            job->request.round, job->members, job->wait_slot && job->owner && job->owner == job->inst->msync, job->request.pull);
    }
    ms_untrack_cleanup(job);
    u_free(job);
}

static int ms_confirm_source(struct ms_session* s, const uint8_t* root, const uint8_t* requested) {
    if (!s->served_root_known || memcmp(root, s->served_root, MT_HASH_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS source changed before confirmation ns=%s round=%llu served=%s current=%s",
            s->ns, (unsigned long long)s->round, sc_public_key_hex(s->served_root, (char[65]){0}), sc_public_key_hex(root, (char[65]){0}));
        return MT_ERR_STALE;
    }
    if (memcmp(root, requested, MT_HASH_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle source confirmation refused ns=%s peer=%s root=%s requested=%s",
            s->ns, sc_public_key_hex(s->peer, (char[65]){0}), sc_public_key_hex(root, (char[65]){0}),
            sc_public_key_hex(requested, (char[65]){0}));
        return MT_ERR_CONFLICT;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS confirmed source ns=%s peer=%s round=%llu root=%s", s->ns,
        sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round, sc_public_key_hex(root, (char[65]){0}));
    return ms_send(s, MS_SOURCE_OK, 0, root, MT_HASH_SIZE);
}

static int ms_sql_finish_walk(struct ms_session* s) {
    if (s->leader) {
        s->state = MS_SERVE; s->served_root_known = s->served_confirmed = 0;
        return ms_send(s, MS_TURN, 0, NULL, 0);
    }
    struct merkle_sync_sql_request request = {.action=MT_SQL_FINISH};
    return ms_sql_submit(s, MS_DB_FINISH_WALK, &request, 0, 0, NULL, 0);
}

/* Вызывается после публикации WRITE, пока сессия всё ещё относится к той же группе. */
static int ms_finish_members(struct ms_session* s) {
    if (ms_round_expired(s)) return MT_ERR_TIMEOUT;
    if (!ms_session_can_run(s) ||
        !ms_registration_current(s->ms->inst, s->ns, s->registration)) return MT_ERR_DISCONNECTED;
    if (s->mode == MS_READER) {
        s->state = MS_COMPLETE;
        s->dirty = 0;
        ms_stop_deadline(s);
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle member pull committed ns=%s peer=%s round=%llu source_root=%s rx=%llu",
            s->ns, sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round,
            sc_public_key_hex(s->source_root, (char[65]){0}), (unsigned long long)s->received_bytes);
        ms_notify(s, MT_OK);
        return MT_OK;
    }
    if (ms_sql_enabled(s)) return ms_sql_finish_walk(s);
    if (s->leader) {
        s->state = MS_SERVE; s->served_root_known = s->served_confirmed = 0;
        return ms_send(s, MS_TURN, 0, NULL, 0);
    }
    uint8_t root[MT_HASH_SIZE];
    if (ms_root(s, root) < 0) return MT_ERR_IO;
    s->state = MS_DONE_WAIT;
    return ms_send(s, MS_CHECK, 0, root, sizeof(root));
}

static int ms_handshake_matches(struct ms_session* s, uint8_t type, uint64_t round,
    const uint8_t* round_id, const uint8_t* accept_id) {
    if (round == s->round && !ms_token_empty(s->round_id) && !memcmp(round_id, s->round_id, MS_TOKEN_SIZE) &&
        (type == MS_READY || (!ms_token_empty(s->accept_id) && !memcmp(accept_id, s->accept_id, MS_TOKEN_SIZE)))) return 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC,
        "merkle_sync: stale handshake ns=%s type=%u round=%llu/%llu token=%016llx/%016llx current=%016llx/%016llx",
        s->ns, type, (unsigned long long)round, (unsigned long long)s->round,
        (unsigned long long)ms_read64(round_id), (unsigned long long)ms_read64(accept_id),
        (unsigned long long)ms_read64(s->round_id), (unsigned long long)ms_read64(s->accept_id));
    return 0;
}

static struct ms_control ms_decode_control(uint8_t type, uint64_t round, uint32_t request, const uint8_t* d, size_t len) {
    struct ms_control c = {.type=type, .round=round};
    memcpy(c.round_id, d + MS_ROUND_ID_OFF, MS_TOKEN_SIZE);
    memcpy(c.accept_id, d + MS_ACCEPT_ID_OFF, MS_TOKEN_SIZE);
    if (type == MS_ERROR) {
        uint32_t error = len == 4 ? ms_read32(d + MS_HEADER) : 0;
        c.valid = !request && round && !ms_token_empty(c.round_id) && !ms_token_empty(c.accept_id) &&
            error >= 1 && error <= (uint32_t)-MT_ERR_LIMIT;
        if (c.valid) c.error = (uint8_t)error;
    } else if (!request && !len && ms_token_empty(c.accept_id))
        c.valid = type == MS_WAKE ? !round && ms_token_empty(c.round_id) : round && !ms_token_empty(c.round_id);
    return c;
}

static int ms_control_ignored(struct ms_session* s, const struct ms_control* c) {
    if (s->state == MS_FAILING) return 1;
    if (c->type == MS_WAKE && s->restart && s->failure_error == MT_ERR_DB_RETRY) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: WAKE already covered by database retry ns=%s", s->ns);
        return 1;
    }
    if (c->type == MS_ERROR) {
        /* Callback завершённого раунда мог уже поставить новый start, которому ещё не назначены токены. */
        if (s->state == MS_IDLE || s->state == MS_COMPLETE || s->state == MS_FAILED || s->state == MS_WAKE_WAIT) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: discard ERROR for inactive round ns=%s state=%d", s->ns, s->state);
            return 1;
        }
        return !ms_handshake_matches(s, c->type, c->round, c->round_id, c->accept_id);
    }
    if (c->type == MS_BEGIN && !memcmp(c->round_id, s->round_id, MS_TOKEN_SIZE)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: discard duplicate BEGIN ns=%s round=%llu", s->ns, (unsigned long long)c->round);
        return 1;
    }
    return 0;
}

/* 0 — команда проигнорирована. 1 — старый шаг замещён, в том числе при ошибке; callback может удалить s. */
static int ms_apply_control(struct ms_session* s, const struct ms_control* c) {
    if (ms_control_ignored(s, c)) return 0;
    if (c->type == MS_ERROR) { ms_fail(s, -(int)c->error, "remote error", 0); return 1; }
    int rc;
    if (c->type == MS_WAKE) {
        ms_discard_tx(s); s->previous_valid = 0;
        rc = ms_begin(s);
    } else {
        uint8_t token[MS_TOKEN_SIZE];
        if (ms_new_token(token) < 0) { ms_fail(s, MT_ERR_IO, "responder token", 1); return 1; }
        ms_cancel_restart(s); /* Принятый BEGIN завершает ожидание повтора WAKE. */
        s->round_deadline_tb = get_time_tb() + MS_ROUND_TIMEOUT_TB;
        ms_discard_tx(s);
        ms_abort_pull(s);
        memcpy(s->round_id, c->round_id, MS_TOKEN_SIZE); memcpy(s->accept_id, token, sizeof(token));
        s->round = c->round; s->state = MS_SERVE; s->waiting = 0;
        s->request_id = s->remote_request_id = 0; s->dirty = 0; s->verified_revision = s->revision;
        s->served_root_known = s->served_confirmed = 0;
        rc = ms_send(s, MS_READY, 0, NULL, 0);
    }
    if (rc < 0) ms_fail(s, MT_ERR_IO, "restart handshake", 1);
    return 1;
}

static void ms_queue_control(struct ms_session* s, struct ms_sql_job* job, const struct ms_control* c) {
    /* До первой значимой команды состояние раунда ещё однозначно. Дальше проверяем строго в порядке получения. */
    if (!job->control_count && ms_control_ignored(s, c)) return;
    if (job->control_count > MS_CONTROL_LIMIT) return;
    if (job->control_count == MS_CONTROL_LIMIT) {
        job->control_count++;
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: SQL control queue full ns=%s round=%llu limit=%u",
            s->ns, (unsigned long long)s->round, MS_CONTROL_LIMIT);
        return;
    }
    job->controls[job->control_count++] = *c;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: queued control ns=%s type=%u round=%llu count=%u",
        s->ns, c->type, (unsigned long long)c->round, job->control_count);
}

/* SQL уже опубликован и отвязан от раунда. Слабая ссылка здесь живёт только до выхода из рассылки callbacks. */
static int ms_drain_controls(struct ms_session* s, struct ms_sql_job* job) {
    if (!job->control_count) return 0;
    s->draining_controls = job; job->session = s;
    int replaced = 0;
    for (unsigned i = 0; job->session && i < job->control_count; i++) {
        if (!ms_sql_current(job)) { replaced = 1; break; }
        if (!ms_session_can_run(s) || s->ms->inst->ntp.startup_wait) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: queued control deferred by policy ns=%s", s->ns);
            break;
        }
        if (job->control_count > MS_CONTROL_LIMIT) {
            replaced = 1; ms_fail(s, MT_ERR_PROTOCOL, "SQL control queue overflow", 1); break;
        }
        replaced |= ms_apply_control(s, &job->controls[i]);
    }
    if (!job->session) return 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: processed SQL controls ns=%s count=%u old_step_replaced=%d",
        s->ns, job->control_count, replaced);
    s->draining_controls = NULL; job->session = NULL;
    return replaced;
}

static int ms_sql_blocked(const struct db_async_result* outcome) {
    int code = outcome->sqlite_code & 255;
    return outcome->status == DB_ASYNC_SQL && (code == SQLITE_BUSY || code == SQLITE_LOCKED);
}

static void ms_sql_done(void* arg, const struct db_async_result* outcome) {
    struct ms_sql_job* job = arg; struct ms_session* s = job->session;
    struct merkle_sync_sql_request* request = &job->request;
    if (job->submission_result) {
        if (outcome->status == DB_ASYNC_FULL && job->wait_slot && !ms_sql_wait(job, "queue full")) {
            *job->submission_result = MT_APPLY_PENDING;
            job->submission_result = NULL;
            return;
        }
        *job->submission_result = request->result < 0 ? request->result : MT_ERR_IO;
        if (s && s->sql_job == job) s->sql_job = NULL;
        job->session = NULL;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL submission refused ns=%s status=%d rc=%d: %s",
            request->ns, outcome->status, outcome->sqlite_code, outcome->message); return;
    }
    if (job->continuation == MS_DB_ABORT) {
        if (job->members && job->cleanup_tracked && ms_sql_blocked(outcome)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL cleanup blocked dataset=%u ns=%s peer=%s round=%llu rc=%d: %s",
                job->dataset, request->ns, sc_public_key_hex(request->peer, (char[65]){0}), (unsigned long long)request->round,
                outcome->sqlite_code, outcome->message);
            if (!ms_sql_wait(job, "staging cleanup blocked")) return;
        }
        if (outcome->status) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC,
            "merkle SQL cleanup deferred to startup ns=%s status=%d rc=%d: %s",
            request->ns, outcome->status, outcome->sqlite_code, outcome->message);
        return;
    }
    int rc = outcome->status ? (request->result < 0 ? request->result : MT_ERR_IO) : request->result;
    if (s && s->mode == MS_EXCHANGE && job->members && rc == MT_ERR_IO && ms_sql_blocked(outcome)) rc = MT_ERR_DB_RETRY;
    if (job->continuation == MS_DB_BEGIN && request->pull) {
        if (outcome->status) {
            /* ID откатившейся вставки может получить соседний pull. При неопределённом COMMIT остаток уберёт startup. */
            job->ops->pull_free(request->pull); request->pull = NULL;
        } else job->owns_pull = 1;
    }
    if (!outcome->status && s && request->action == MT_SQL_COMMIT_PULL && request->result != MT_APPLY_PENDING) {
        if (s->pull == request->pull) s->pull = NULL;
        job->owns_pull = request->pull != NULL;
    }
    struct NODE_CONN_DIRECT* transport = NULL;
    if (!outcome->status && s && !job->members && job->continuation == MS_DB_COMMIT_READER && request->result != MT_APPLY_PENDING) {
        transport = s->transport; s->transport = NULL; /* Допуск публикуется до передачи удержания готовой группе. */
    }
    if (!outcome->status && s && request->result == MT_APPLY_PENDING)
        s->state = job->continuation == MS_DB_COMMIT_READER ? MS_READER_APPLY_WAIT : MS_APPLY_WAIT;
    /* Durable изменения публикуются даже после отмены отдельной сессии, пока живо то же поколение сервиса. */
    if (!outcome->status && ms_sql_current(job) && job->model && job->ops->publish) {
        struct ms_dataset* dataset = &job->owner->datasets[job->dataset];
        if (dataset->ops && dataset->ops->sql == job->ops) job->ops->publish(dataset->data_ctx, job->scope, request);
    }
    node_conn_direct_close(transport); s = job->session;
    if (!s) return;
    if (!ms_sql_current(job)) {
        if (s->sql_job == job) {
            if (request->pull && s->pull == request->pull) { s->pull = NULL; job->owns_pull = 1; }
            s->sql_job = NULL; s->sql_resume = 0;
        }
        job->session = NULL; return;
    }
    int resume = s->sql_resume; s->sql_resume = 0; s->sql_job = NULL; job->session = NULL;
    if (rc != MT_ERR_LIMIT && rc != MT_ERR_DB_RETRY && ms_round_expired(s)) { ms_fail(s, MT_ERR_TIMEOUT, "database completion", 0); return; }
    if (rc < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC,
            "merkle SQL failed dataset=%u ns=%s peer=%s round=%llu request=%u step=%d action=%d status=%d rc=%d: %s",
            job->dataset, request->ns, sc_public_key_hex(request->peer, (char[65]){0}), (unsigned long long)request->round,
            job->wire_request, job->continuation, request->action, outcome->status, outcome->sqlite_code, outcome->message);
        ms_fail(s, rc, "database operation", 1); return;
    }
    if (job->continuation == MS_DB_BEGIN) {
        if (!request->pull) { ms_fail(s, MT_ERR_IO, "model returned no pull", 1); return; }
        s->pull = request->pull; job->owns_pull = 0;
    }
    if (ms_drain_controls(s, job)) return;
    if (request->result == MT_APPLY_PENDING) {
        rc = s->dataset->ops->dependency ? ms_query_dependency(s) : resume ? ms_schedule(s) : ms_progress(s);
        if (rc < 0) ms_fail(s, rc, "pending application scheduling", 1);
        return;
    }
    switch (job->continuation) {
    case MS_DB_SERVE_HASHES:
        if (job->read_pinned_root) {
            memcpy(s->served_root, job->read_root, MT_HASH_SIZE);
            s->served_root_known = 1;
        }
        rc = ms_send(s, MS_HASHES, job->wire_request, &job->hashes[0][0], sizeof(job->hashes)); break;
    case MS_DB_SERVE_PAGE: {
        uint8_t page[MT_PAGE_SIZE - MS_HEADER];
        rc = ms_page_output(s, request->prefix, request->after_valid ? &request->after : NULL, &request->next,
            request->more, request->length, ms_frame_limit(s) - MS_HEADER - MS_PAGE_HEADER);
        if (rc) break;
        page[0] = (uint8_t)request->more;
        memcpy(page + 1, request->next.bytes, MT_KEY_SIZE); memcpy(page + MS_PAGE_HEADER, request->data, request->length);
        rc = ms_send(s, MS_PAGE, job->wire_request, page, request->length + MS_PAGE_HEADER); break;
    }
    case MS_DB_COMPARE_HASHES: rc = ms_compare_hashes(s, &job->hashes[0][0], job->remote); break;
    case MS_DB_APPLY_PAGE: rc = ms_page_applied(s, request); break;
    case MS_DB_SERVE_DEPENDENCY:
        rc = !request->length || request->length > MT_PAGE_SIZE - MS_HEADER ? MT_ERR_DATA :
            ms_send(s, MS_DEP_REPLY, job->wire_request, request->data, request->length); break;
    case MS_DB_APPLY_DEPENDENCY: rc = ms_dependency_applied(s); break;
    case MS_DB_BEGIN:
        if (job->members) memcpy(s->local_root, job->read_root, MT_HASH_SIZE);
        s->state = MS_PULL; s->depth = 0; s->after_valid = 0; memset(&s->after, 0, sizeof(s->after));
        memset(s->stack, 0, sizeof(s->stack)); rc = ms_query(s); break;
    case MS_DB_COMMIT_WALK:
    case MS_DB_COMMIT_READER:
        if (job->members) { rc = ms_finish_members(s); break; }
        if (job->continuation == MS_DB_COMMIT_WALK) { rc = ms_sql_finish_walk(s); break; }
        s->state = MS_COMPLETE; s->dirty = 0; ms_stop_deadline(s); ms_notify(s, MT_OK); return;
    case MS_DB_FINISH_WALK:
        rc = ms_sql_submit(s, MS_DB_SEND_CHECK, NULL, 0, 0, NULL, 0); break;
    case MS_DB_SEND_CHECK:
        s->state = MS_DONE_WAIT; rc = ms_send(s, MS_CHECK, 0, job->hashes[0], MT_HASH_SIZE); break;
    case MS_DB_CONFIRM_SOURCE: rc = ms_confirm_source(s, job->hashes[0], job->remote); break;
    case MS_DB_COMPARE_CHECK:
        if (!memcmp(job->hashes[0], job->remote, MT_HASH_SIZE)) {
            struct merkle_sync_sql_request finish = {.action=MT_SQL_FINISH};
            rc = ms_sql_submit(s, MS_DB_FINISH_DONE, &finish, 0, 0, job->hashes[0], MT_HASH_SIZE);
        } else rc = ms_retry_root_mismatch(s, job->hashes[0], job->remote);
        break;
    case MS_DB_FINISH_DONE:
        s->previous_valid = 0;
        if (s->verified_revision != s->revision)
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle root changed before DONE ns=%s checked=%llu current=%llu; another round required",
                s->ns, (unsigned long long)s->verified_revision, (unsigned long long)s->revision);
        rc = ms_send(s, MS_DONE, 0, job->remote, MT_HASH_SIZE); break;
    case MS_DB_COMPARE_DONE:
        s->state = MS_IDLE; ms_stop_deadline(s);
        s->dirty = s->verified_revision != s->revision || memcmp(job->hashes[0], job->remote, MT_HASH_SIZE) != 0;
        if (s->dirty) rc = ms_schedule(s); else { ms_notify(s, MT_OK); return; }
        break;
    default: rc = MT_ERR_PROTOCOL; break;
    }
    if (rc < 0) ms_fail(s, rc, "database continuation", 1);
}

static int ms_sql_wait(struct ms_sql_job* job, const char* reason) {
    unsigned delay = job->retry_delay ? job->retry_delay : MS_SQL_RETRY_MIN_TB;
    job->retry_timer = uasync_set_timeout(job->inst->ua, delay, job, ms_sql_retry, "merkle_sql_retry");
    if (!job->retry_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL retry timer allocation ns=%s action=%u", job->request.ns, job->request.action);
        return MT_ERR_IO;
    }
    job->retry_delay = delay < MS_SQL_RETRY_MAX_TB / 2 ? delay * 2 : MS_SQL_RETRY_MAX_TB;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL retry deferred ns=%s round=%llu request=%u action=%u delay_tb=%u: %s",
        job->request.ns, (unsigned long long)job->request.round, job->wire_request, job->request.action, delay, reason);
    return MT_OK;
}
static int ms_sql_run(struct ms_sql_job* job) {
    enum merkle_sync_sql_action action = job->request.action;
    int submission_result = 0;
    job->submission_result = &submission_result;
    db_async_run(job->backend, &(struct db_async_task){
        .name=job->continuation == MS_DB_ABORT ? "merkle-cleanup" : "merkle-step",
        .mode=!job->model || action == MT_SQL_PAGE || action == MT_SQL_SERVE_DEPENDENCY ? DB_ASYNC_READ : DB_ASYNC_WRITE,
        .guard=job->continuation == MS_DB_ABORT ? NULL : ms_sql_guard,
        .work=ms_sql_work, .done=ms_sql_done, .cleanup=ms_sql_cleanup, .data=job});
    if (!submission_result) job->submission_result = NULL;
    return submission_result < 0 ? submission_result : MT_OK;
}
static void ms_sql_retry(void* arg) {
    struct ms_sql_job* job = arg;
    struct ms_session* s = job->session;
    job->retry_timer = NULL;
    if (s && ms_round_expired(s)) {
        s->sql_job = NULL;
        ms_sql_cleanup(job);
        ms_fail(s, MT_ERR_TIMEOUT, "database retry", 0);
        return;
    }
    if (job->inst->db_worker != job->backend || (s && (!ms_sql_current(job) ||
        !ms_session_can_run(s)))) {
        if (s) s->sql_job = NULL;
        ms_sql_cleanup(job);
        if (s) ms_fail(s, MT_ERR_DISCONNECTED, "database retry no longer allowed", 0);
        return;
    }
    if (s && job->continuation == MS_DB_BEGIN && ms_cleanup_pending(s)) {
        if (!ms_sql_wait(job, "previous staging cleanup pending")) return;
        s->sql_job = NULL;
        ms_sql_cleanup(job);
        ms_fail(s, MT_ERR_IO, "waiting for previous pull cleanup", 1);
        return;
    }
    if (s) s->publishing = &s;
    int rc = ms_sql_run(job);
    if (s) {
        s->publishing = NULL;
        if (rc < 0) ms_fail(s, rc, "database retry submission", 1);
    }
}
static int ms_sql_submit(struct ms_session* s, enum ms_sql_continuation continuation,
    struct merkle_sync_sql_request* request, uint8_t level, uint32_t wire_request, const uint8_t* remote, size_t remote_size) {
    if (ms_round_expired(s)) return MT_ERR_TIMEOUT;
    int model = request && continuation != MS_DB_SERVE_HASHES;
    if (s->sql_job || (model && (!s->dataset->ops->sql || !s->dataset->ops->sql->work)) || remote_size > MT_BUCKETS * MT_HASH_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL invalid/busy model dataset=%u ns=%s busy=%d",
            s->dataset->id, s->ns, s->sql_job != NULL); return MT_ERR_IO;
    }
    struct ms_sql_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL allocation ns=%s", s->ns); return MT_ERR_IO; }
    job->inst = s->ms->inst; job->backend = job->inst->db_worker;
    job->owner = s->ms; job->session = s; job->ops = s->dataset->ops->sql;
    job->wait_slot = ms_members(s) || s->dataset->ops->dependency != NULL;
    job->continuation = continuation; job->model = model; job->level = level; job->wire_request = wire_request;
    job->generation = job->inst->group_generation; job->dataset = s->dataset->id;
    job->members = ms_members(s); job->registration = s->registration;
    if (request) job->request = *request;
    else job->request.prefix = s->depth >= 0 ? s->stack[s->depth].prefix : 0;
    strcpy(job->request.ns, s->ns); strcpy(job->tree_ns, s->tree_ns);
    memcpy(job->request.peer, s->peer, 32); job->request.round = s->round;
    job->peer_node = s->peer_node;
    if (job->members) {
        if (continuation == MS_DB_BEGIN || (continuation == MS_DB_SERVE_HASHES && !s->served_root_known)) {
            job->read_pinned_root = 1;
        } else if (continuation == MS_DB_SERVE_HASHES || continuation == MS_DB_SERVE_PAGE ||
                   continuation == MS_DB_COMPARE_HASHES || continuation == MS_DB_COMMIT_WALK || continuation == MS_DB_COMMIT_READER) {
            const uint8_t* root = continuation == MS_DB_SERVE_HASHES || continuation == MS_DB_SERVE_PAGE ? s->served_root : s->local_root;
            job->check_pinned_root = 1;
            memcpy(job->pinned_root, root, MT_HASH_SIZE);
        }
    }
    if (remote_size) memcpy(job->remote, remote, remote_size);
    s->sql_job = job;
    if (continuation == MS_DB_BEGIN && ms_cleanup_pending(s)) {
        if (!ms_sql_wait(job, "previous staging cleanup pending")) return MT_OK;
        s->sql_job = NULL;
        ms_sql_cleanup(job);
        return MT_ERR_IO;
    }
    return ms_sql_run(job); /* При отказе handler завершит сессию после возврата из submit. */
}

static void ms_sql_abort(struct UTUN_INSTANCE* inst, const struct merkle_sync_sql_ops* ops,
    const char* ns, const uint8_t peer[32], uint8_t dataset, uint64_t round, int members, int wait_slot, void* pull) {
    if (!ops || !ops->work || !ops->pull_free) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL missing pull cleanup model ns=%s", ns); return;
    }
    struct ms_sql_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL cleanup allocation ns=%s; staging remains until startup", ns);
        ops->pull_free(pull); return;
    }
    job->inst = inst; job->backend = inst->db_worker; job->owner = inst->msync;
    job->ops = ops; job->model = 1; job->owns_pull = 1; job->continuation = MS_DB_ABORT;
    job->dataset = dataset; job->members = members; job->wait_slot = wait_slot && job->owner;
    job->request.action = MT_SQL_ABORT_PULL; job->request.pull = pull; strcpy(job->request.ns, ns);
    memcpy(job->request.peer, peer, 32); job->request.round = round;
    ms_track_cleanup(job);
    if (ms_sql_run(job) < 0) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle SQL cleanup could not be queued ns=%s", ns);
}

static void ms_receive_frame(struct UTUN_INSTANCE* inst, struct ETCP_CONN* conn, const struct chat_join_path* join,
    struct ll_entry* entry) {
    if (!entry) return;
    if (inst && inst->ntp.startup_wait) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle receive waits for initial time");
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    const uint8_t* peer = ms_remote_key(inst, conn, join);
    if (!ms || !entry->dgram || entry->len < MS_HEADER || entry->len > (join ? CHAT_JOIN_BODY_MAX : MT_PAGE_SIZE) || entry->dgram[0] != ms->svc_id) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: invalid packet size=%u", entry->len);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    const uint8_t* d = entry->dgram;
    uint64_t ns_id, round = ms_read64(d + MS_ROUND_OFF);
    uint32_t request = ms_read32(d + MS_REQUEST_OFF);
    uint8_t type = d[MS_TYPE_OFF] & ~(MS_READ_ONLY | MS_RESPONSE), dataset = d[MS_DATASET_OFF];
    uint8_t mode = d[MS_TYPE_OFF] & MS_READ_ONLY ? (d[MS_TYPE_OFF] & MS_RESPONSE ? MS_READER : MS_WRITER) : MS_EXCHANGE;
    struct ms_dataset* model = &ms->datasets[dataset];
    if ((join && (dataset != MT_DATA_MEMBERS || mode != (!memcmp(inst->my_ed25519_pubkey, join->inviter, 32) ? MS_WRITER : MS_READER))) ||
        !model->ops || ((d[MS_TYPE_OFF] & MS_RESPONSE) && mode == MS_EXCHANGE) ||
        (mode != MS_EXCHANGE && !model->ops->validate_request)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: unsupported dataset=%u flags=%02x", dataset, d[MS_TYPE_OFF]);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    const uint8_t* key = d + MS_SCOPE_OFF;
    struct TOPO_GROUP* group = model->ops->group_scoped ? topo_groups_find_wire(ms->inst->topo_groups, key) : NULL;
    if (model->ops->group_scoped ? (!group || group->group_type != TOPO_GROUP_TYPE_CHAT)
                                 : memcmp(key + 8, (uint8_t[24]){0}, 24) != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: unknown group dataset=%u key=%s peer=%s",
            dataset, sc_public_key_hex(key, (char[65]){0}), sc_public_key_hex(peer, (char[65]){0}));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    ns_id = group ? group->group_id : ms_read64(key);
    char ns[65];
    if (group) strcpy(ns, group->channel_id);
    else snprintf(ns, sizeof(ns), "%llu", (unsigned long long)ns_id);
    if (join ? !chat_sync_join_members_allowed(inst, join, ns) : !ms_allowed(model, conn, ns, mode)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: unauthorized dataset=%u ns=%s peer=%s type=%u",
            dataset, ns, sc_public_key_hex(peer, (char[65]){0}), type);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    if (!join && !ms_can_run(model, conn, ns, mode)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: model receive deferred dataset=%u ns=%s peer=%s",
            dataset, ns, sc_public_key_hex(peer, (char[65]){0}));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    struct ms_session* s = ms_find(ms, conn, join, dataset, ns_id, mode);
    const uint8_t* p = d + MS_HEADER;
    size_t len = entry->len - MS_HEADER;
    int is_control = type == MS_BEGIN || type == MS_WAKE || type == MS_ERROR;
    struct ms_control control;
    if (is_control) {
        control = ms_decode_control(type, round, request, d, len);
        int leader = s ? s->leader : ms_local_leads(ms, conn, mode);
        if (!control.valid || (type == MS_WAKE && (mode != MS_EXCHANGE || !leader)) || (type == MS_BEGIN && leader)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC,
                "merkle_sync: invalid control dataset=%u ns=%s peer=%s attempt=%016llx type=%u round=%llu request=%u bytes=%zu mode=%u leader=%d",
                dataset, ns, sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)(join ? join->attempt : 0),
                type, (unsigned long long)round, request, len, mode, leader);
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
    }
    if (!s && (type == MS_WAKE || type == MS_BEGIN))
        s = ms_create(ms, conn, join, dataset, ns_id, mode);
    if (!s) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: no session ns=%s type=%u", ns, type);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    if (ms_round_expired(s)) {
        queue_dgram_free(entry); queue_entry_free(entry);
        ms_fail(s, MT_ERR_TIMEOUT, "packet receive", 0);
        return;
    }
    s->received_bytes += entry->len;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC,
        "merkle_sync: RX self=%s dataset=%u ns=%s peer=%s round=%llu type=%u request=%u bytes=%zu state=%d",
        sc_public_key_hex(ms->inst->my_ed25519_pubkey, (char[65]){0}), dataset, ns, sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)round,
        type, request, len, s->state);
    if (is_control) {
        queue_dgram_free(entry); queue_entry_free(entry);
        struct ms_sql_job* busy = s->draining_controls ? s->draining_controls : s->sql_job;
        if (busy && !busy->retry_timer) ms_queue_control(s, busy, &control);
        else ms_apply_control(s, &control);
        return;
    }
    if (s->sql_job || s->draining_controls) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: discard duplicate while SQL pending ns=%s type=%u", ns, type);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    int rc = 0, notify = 0;
    if (s->state == MS_FAILING || s->state == MS_FAILED || s->state == MS_WAKE_WAIT ||
        !ms_handshake_matches(s, type, round, d + MS_ROUND_ID_OFF, d + MS_ACCEPT_ID_OFF)) goto done;
    if (type == MS_READY) {
        if (!s->leader || request || len || ms_token_empty(d + MS_ACCEPT_ID_OFF)) rc = MT_ERR_PROTOCOL;
        else if (s->state == MS_BEGIN_WAIT) {
            memcpy(s->accept_id, d + MS_ACCEPT_ID_OFF, MS_TOKEN_SIZE);
            rc = ms_pull_start(s);
        } else if (!memcmp(d + MS_ACCEPT_ID_OFF, s->accept_id, MS_TOKEN_SIZE)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: discard duplicate READY ns=%s", ns);
        } else rc = MT_ERR_PROTOCOL;
    } else if (type == MS_QUERY) rc = ms_serve_query(s, request, p, len);
    else if (type == MS_HASHES) rc = ms_receive_hashes(s, request, p, len);
    else if (type == MS_PAGE) rc = ms_receive_page(s, request, p, len);
    else if (type == MS_DEP_QUERY) rc = ms_serve_dependency(s, request, p, len);
    else if (type == MS_DEP_REPLY) {
        rc = ms_receive_dependency(&s, request, p, len);
        if (!s) { queue_dgram_free(entry); queue_entry_free(entry); return; }
    }
    else if (type == MS_TURN) {
        if (mode != MS_EXCHANGE || s->leader || s->state != MS_SERVE || request || len || (ms_members(s) && !s->served_confirmed))
            rc = MT_ERR_PROTOCOL;
        else rc = ms_pull_start(s);
    } else if (type == MS_SOURCE_CHECK) {
        if (!ms_members(s) || mode == MS_READER || s->state != MS_SERVE || request || len != MT_HASH_SIZE || !s->served_root_known)
            rc = MT_ERR_PROTOCOL;
        else if (ms_sql_enabled(s)) rc = ms_sql_submit(s, MS_DB_CONFIRM_SOURCE, NULL, 0, 0, p, len);
        else {
            uint8_t root[MT_HASH_SIZE];
            rc = ms_root(s, root) < 0 ? MT_ERR_IO : ms_confirm_source(s, root, p);
        }
    } else if (type == MS_SOURCE_OK) {
        if (!ms_members(s) || mode == MS_WRITER || s->state != MS_SOURCE_WAIT || request || len != MT_HASH_SIZE || !s->pull)
            rc = MT_ERR_PROTOCOL;
        else if (memcmp(s->source_root, p, MT_HASH_SIZE)) rc = MT_ERR_CONFLICT;
        else if (ms_sql_enabled(s)) {
            struct merkle_sync_sql_request query = {.action=MT_SQL_COMMIT_PULL, .pull=s->pull};
            rc = ms_sql_submit(s, mode == MS_READER ? MS_DB_COMMIT_READER : MS_DB_COMMIT_WALK, &query, 0, 0, NULL, 0);
        } else {
            rc = ms_check_root(ms->inst->topo_sqlite_db, s->tree_ns, s->local_root, ns, s->round, NULL);
            if (rc >= 0) rc = ms_commit_pull(s);
            if (rc >= 0) s->state = MS_MEMBERS_COMMITTED;
        }
    } else if (type == MS_CHECK && mode == MS_WRITER) {
        if (s->state != MS_SERVE || request || len || ms_members(s)) rc = MT_ERR_PROTOCOL;
        else if (s->revision != s->verified_revision) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: pull source changed ns=%s peer=%s revision=%llu/%llu",
                ns, sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->revision, (unsigned long long)s->verified_revision);
            rc = MT_ERR_CONFLICT;
        } else rc = ms_send(s, MS_DONE, 0, NULL, 0);
    } else if (type == MS_CHECK) {
        uint8_t roots[MT_HASH_SIZE];
        if (mode != MS_EXCHANGE || !s->leader || s->state != MS_SERVE || request || len != MT_HASH_SIZE || (ms_members(s) && !s->served_confirmed))
            rc = MT_ERR_PROTOCOL;
        else if (ms_sql_enabled(s)) { rc = ms_sql_submit(s, MS_DB_COMPARE_CHECK, NULL, 0, 0, p, len); goto done; }
        else if (ms_root(s, roots) < 0) rc = MT_ERR_IO;
        else if (!memcmp(roots, p, MT_HASH_SIZE)) {
            if (model->ops->finish && model->ops->finish(model->data_ctx, ns) < 0) rc = MT_ERR_DATA;
            else {
                s->previous_valid = 0; s->verified_revision = s->revision;
                rc = ms_send(s, MS_DONE, 0, roots, MT_HASH_SIZE) < 0 ? MT_ERR_IO : 0;
            }
        } else rc = ms_retry_root_mismatch(s, roots, p);
    } else if (type == MS_DONE && mode == MS_READER) {
        if (s->state != MS_DONE_WAIT || request || len || !s->pull || ms_members(s)) rc = MT_ERR_PROTOCOL;
        else if (ms_sql_enabled(s)) {
            struct merkle_sync_sql_request query = {.action=MT_SQL_COMMIT_PULL, .pull=s->pull};
            rc = ms_sql_submit(s, MS_DB_COMMIT_READER, &query, 0, 0, NULL, 0);
        }
        else {
            rc = ms_commit_pull(s);
            if (ms_round_expired(s)) rc = MT_ERR_TIMEOUT;
            else if (rc == MT_APPLY_PENDING) { s->state = MS_READER_APPLY_WAIT; rc = ms_progress(s); }
            else if (rc >= 0) {
                s->state = MS_COMPLETE; s->dirty = 0;
                ms_stop_deadline(s);
                DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: pull accepted dataset=%u ns=%s peer=%s round=%llu",
                    dataset, ns, sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round);
                notify = 1;
            }
        }
    } else if (type == MS_DONE) {
        uint8_t root[MT_HASH_SIZE];
        if (mode != MS_EXCHANGE || s->leader || s->state != MS_DONE_WAIT || request || len != MT_HASH_SIZE) rc = MT_ERR_PROTOCOL;
        else if (ms_sql_enabled(s)) { rc = ms_sql_submit(s, MS_DB_COMPARE_DONE, NULL, 0, 0, p, len); goto done; }
        else if (ms_root(s, root) < 0) rc = MT_ERR_IO;
        else if (ms_round_expired(s)) rc = MT_ERR_TIMEOUT;
        else {
            s->state = MS_IDLE;
            ms_stop_deadline(s);
            s->dirty = memcmp(root, p, sizeof(root)) != 0;
            if (s->dirty) rc = ms_schedule(s);
            else {
                DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: converged dataset=%u ns=%s peer=%s round=%llu",
                    s->dataset->id, s->ns, sc_public_key_hex(s->peer, (char[65]){0}), (unsigned long long)s->round);
                notify = 1;
            }
        }
    } else rc = MT_ERR_PROTOCOL;
done:
    if (rc != MT_ERR_LIMIT && ms_round_expired(s)) rc = MT_ERR_TIMEOUT;
    void* pending=s->publish_pending?s->pull:NULL; s->publish_pending=0;
    void* finished = s->finished_pull; s->finished_pull = NULL;
    struct NODE_CONN_DIRECT* transport = notify ? s->transport : NULL;
    if (notify) s->transport = NULL; /* Публикация MEMBERS ниже должна передать владение группе до release. */
    if (transport) DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: retaining transport through publication dataset=%u ns=%s",
        s->dataset->id, s->ns);
    const struct merkle_sync_data_ops* ops = model->ops;
    void* data_ctx = model->data_ctx;
    queue_dgram_free(entry); queue_entry_free(entry);
    if (finished && ms_members(s)) {
        struct ms_session* live = s;
        s->publishing = &live;
        ops->publish_pull(data_ctx, finished);
        ops->abort_pull(data_ctx, finished);
        if (live) {
            live->publishing = NULL;
            if (live->state == MS_MEMBERS_COMMITTED) {
                rc = ms_finish_members(live);
                if (rc < 0) ms_fail(live, rc, "member receive completion", 1);
            }
        }
        return;
    }
    if (rc < 0) ms_fail(s, rc, "receive", 1);
    else if (notify) ms_notify(s, MT_OK);
    if(pending && rc>=0 && ops->publish_pull) ops->publish_pull(data_ctx,pending);
    if (finished) { ops->publish_pull(data_ctx, finished); ops->abort_pull(data_ctx, finished); }
    node_conn_direct_close(transport);
}

static void ms_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    ms_receive_frame(conn ? conn->instance : NULL, conn, NULL, entry);
}

void merkle_sync_join_receive(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const uint8_t* data, size_t size) {
    if (size < MS_HEADER || size > CHAT_JOIN_BODY_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN MEMBERS invalid frame bytes=%zu", size);
        return;
    }
    struct ll_entry* entry = ll_alloc_lldgram(size);
    if (!entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN MEMBERS frame allocation failed bytes=%zu", size);
        return;
    }
    memcpy(entry->dgram, data, size); entry->len = size;
    ms_receive_frame(inst, NULL, path, entry);
}

static void ms_conn_status(struct ETCP_CONN* conn, int event, void* arg) {
    struct merkle_sync* ms = arg;
    if (event != ETCP_CONN_STATUS_DOWN && event != ETCP_CONN_STATUS_DELETE && event != ETCP_CONN_STATUS_REINIT) return;
    ms->closing_conn = conn;
    /* DELETE приходит до освобождения normalizer/input. Отмена использует сохранённую очередь. */
    for (;;) {
        struct ms_session* s = ms->sessions;
        while (s && s->conn != conn) s = s->next;
        if (!s) break;
        /* Удаляем до callback, чтобы повторный start не мог сохранить умирающую сессию. */
        struct ms_request* requests = s->requests; s->requests = NULL;
        uint8_t peer[32]; memcpy(peer, s->peer, 32); char ns[sizeof(s->ns)]; memcpy(ns, s->ns, sizeof(ns));
        void (*on_result)(void*, const char*, const uint8_t*, int) = s->mode == MS_WRITER ? NULL : s->dataset->ops->on_result;
        void* data_ctx = s->dataset->data_ctx;
        ms_free(s);
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: detach ns=%s peer=%s event=%d", ns, sc_public_key_hex(peer, (char[65]){0}), event);
        if (on_result) on_result(data_ctx, ns, peer, MT_ERR_DISCONNECTED);
        while (requests) {
            struct ms_request* next = requests->next;
            merkle_sync_done_cb cb = requests->cb; void* cb_arg = requests->arg;
            u_free(requests); cb(peer, ns, MT_ERR_DISCONNECTED, cb_arg); requests = next;
        }
    }
    ms->closing_conn = NULL;
}

int merkle_sync_init(struct UTUN_INSTANCE* inst, uint8_t svc_id, const struct merkle_sync_data_ops* ops, void* data_ctx) {
    if (!inst || inst->msync || !inst->ua || !inst->topo_sqlite_db) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: invalid initialization"); return -1;
    }
    if (merkle_tree_init(inst->topo_sqlite_db) < 0) return -1;
    return merkle_sync_activate(inst, svc_id, ops, data_ctx);
}

int merkle_sync_activate(struct UTUN_INSTANCE* inst, uint8_t svc_id, const struct merkle_sync_data_ops* ops, void* data_ctx) {
    if (!inst || inst->msync || !inst->ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: invalid runtime activation"); return -1;
    }
    struct merkle_sync* ms = u_calloc(1, sizeof(*ms));
    if (!ms) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: allocation failed"); return -1; }
    ms->inst = inst; ms->svc_id = svc_id;
    /* Round counter is local to a connection lifetime; monotonic across session cancellation. */
    ms->round_seq = get_time_tb();
    inst->msync = ms;
    if (merkle_sync_register(inst, MT_DATA_MEMBERS, ops, data_ctx) < 0) { inst->msync = NULL; u_free(ms); return -1; }
    etcp_bind(inst, svc_id, ms_receive);
    etcp_add_conn_status_cbk(inst, ms_conn_status, ms);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: initialized service=%u protocol=5", svc_id);
    return 0;
}

/* Регистрирует независимую модель в общем движке. */
int merkle_sync_register(struct UTUN_INSTANCE* inst, uint8_t dataset, const struct merkle_sync_data_ops* ops, void* data_ctx) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms || ms->datasets[dataset].ops || !ops || ops->cursor_zero_tail > MT_KEY_SIZE - 8 || !ops->update_bucket_hash || !ops->get_page
        || (!ops->apply_items && !ops->apply_page && !ops->stage_page && !ops->stage_page_request) || (ops->validate_request && !ops->begin_pull)
        || (ops->dependency && (!ops->begin_pull || !ops->dependency_work || ops->validate_request))
        || ((ops->begin_pull || ops->commit_pull || ops->abort_pull || ops->stage_page || ops->stage_page_request || ops->publish_pull)
            && !(ops->begin_pull && ops->commit_pull && ops->abort_pull && (ops->stage_page || ops->stage_page_request)))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: invalid dataset registration dataset=%u", dataset); return -1;
    }
    ms->datasets[dataset] = (struct ms_dataset){ ops, data_ctx, dataset };
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: registered dataset=%u", dataset);
    return 0;
}

int merkle_sync_registered(struct UTUN_INSTANCE* inst, uint8_t dataset) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    return ms && ms->datasets[dataset].ops;
}

void merkle_sync_unregister(struct UTUN_INSTANCE* inst, uint8_t dataset) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms || !ms->datasets[dataset].ops) return;
    if (dataset == MT_DATA_MEMBERS) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: MEMBERS requires engine destroy"); return;
    }
    for (;;) {
        struct ms_session* selected = NULL;
        for (struct ms_session* s = ms->sessions; s; s = s->next) if (s->dataset->id == dataset) { selected = s; break; }
        if (!selected) break;
        ms_free(selected);
    }
    memset(&ms->datasets[dataset], 0, sizeof(ms->datasets[dataset]));
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: unregistered dataset=%u", dataset);
}

void merkle_sync_destroy(struct UTUN_INSTANCE* inst) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms) return;
    etcp_remove_conn_status_cbk(inst, ms_conn_status, ms);
    etcp_unbind(inst, ms->svc_id);
    while (ms->sessions) ms_free(ms->sessions);
    while (ms->cleanups) {
        struct ms_sql_job* job = ms->cleanups;
        ms_untrack_cleanup(job);
        job->owner = NULL;
        if (!job->retry_timer) continue;
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "merkle staging cleanup handed to startup ns=%s", job->request.ns);
        uasync_cancel_timeout(inst->ua, job->retry_timer); job->retry_timer = NULL;
        ms_sql_cleanup(job);
    }
    inst->msync = NULL; u_free(ms);
}

static int ms_start(struct UTUN_INSTANCE* inst, uint8_t dataset, struct ETCP_CONN* conn, const struct chat_join_path* join, const char* ns,
        merkle_sync_done_cb cb, void* arg, int pull_only) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    const uint8_t* peer = ms_remote_key(inst, conn, join);
    unsigned long long attempt = join ? join->attempt : 0;
    uint64_t ns_id = 0;
    if (!ms || !ms->datasets[dataset].ops || !ns ||
        (ms->datasets[dataset].ops->group_scoped ? !(ns_id = topo_groups_channel_slot(inst->topo_groups, ns)) : ms_namespace(ns, &ns_id) < 0) || (!join && (!conn || conn->instance != inst || !conn->initialized || !conn->links_up || conn->state == 2
        || conn->close_requested || !memcmp(inst->my_ed25519_pubkey, conn->peer_ed25519_pubkey, 32) || conn == ms->closing_conn))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: start unavailable ns=%s peer=%s attempt=%016llx", ns ? ns : "-",
            sc_public_key_hex(peer, (char[65]){0}), attempt);
        return -1;
    }
    uint8_t mode = !join && !pull_only && ms_allowed(&ms->datasets[dataset], conn, ns, MS_EXCHANGE) ? MS_EXCHANGE : MS_READER;
    if (join ? !chat_sync_join_members_allowed(inst, join, ns) : !ms_can_run(&ms->datasets[dataset], conn, ns, mode)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: start denied/deferred dataset=%u ns=%s peer=%s attempt=%016llx mode=%u",
            dataset, ns, sc_public_key_hex(peer, (char[65]){0}), attempt, mode); return -1;
    }
    struct ms_request* r = NULL;
    if (cb) {
        r = u_calloc(1, sizeof(*r));
        if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: request allocation failed"); return -1; }
        r->cb = cb; r->arg = arg;
    }
    struct ms_session* s = ms_find(ms, conn, join, dataset, ns_id, mode);
    if (!s) s = ms_create(ms, conn, join, dataset, ns_id, mode);
    if (!s) { u_free(r); return -1; }
    if (s->state == MS_FAILING) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "merkle_sync: start during terminal handoff ns=%s", ns);
        u_free(r); return -1;
    }
    if ((s->state == MS_FAILED && !s->restart) || s->state == MS_COMPLETE) { s->state = MS_IDLE; s->previous_valid = 0; }
    s->dirty = 1;
    if (s->state == MS_IDLE && ms_schedule(s) < 0) {
        u_free(r); ms_fail(s, MT_ERR_IO, "start scheduling", 1); return -1;
    }
    if (r) { r->next = s->requests; s->requests = r; }
    return 0;
}

int merkle_sync_start(struct UTUN_INSTANCE* inst, uint8_t dataset, struct ETCP_CONN* conn, const char* ns,
        merkle_sync_done_cb cb, void* arg) {
    return ms_start(inst, dataset, conn, NULL, ns, cb, arg, 0);
}

int merkle_sync_pull(struct UTUN_INSTANCE* inst, uint8_t dataset, struct ETCP_CONN* conn, const char* ns,
        merkle_sync_done_cb cb, void* arg) {
    return ms_start(inst, dataset, conn, NULL, ns, cb, arg, 1);
}

int merkle_sync_join_pull(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* ns,
    merkle_sync_done_cb cb, void* arg) {
    return ms_start(inst, MT_DATA_MEMBERS, NULL, path, ns, cb, arg, 1);
}

void merkle_sync_join_cancel(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* ns) {
    struct merkle_sync* ms = inst->msync;
    if (!ms) return;
    uint64_t id = topo_groups_channel_slot(inst->topo_groups, ns);
    struct ms_session* s = ms_find(ms, NULL, path, MT_DATA_MEMBERS, id, MS_READER);
    if (s) ms_free(s);
}

static void ms_cancel_peer_scope(struct UTUN_INSTANCE* inst, uint8_t dataset, const uint8_t peer[32], const char* ns, int reader) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms || !ns) return;
    struct ms_session* s = ms->sessions;
    while (s) {
        if (s->dataset->id == dataset && (!reader || s->mode == MS_READER) && peer && !memcmp(s->peer, peer, 32) && !strcmp(s->ns, ns)) {
            ms_free(s); s = ms->sessions;
        }
        else s = s->next;
    }
}

void merkle_sync_cancel(struct UTUN_INSTANCE* inst, uint8_t dataset, const uint8_t peer[32], const char* ns) {
    ms_cancel_peer_scope(inst, dataset, peer, ns, 0);
}

void merkle_sync_cancel_pull(struct UTUN_INSTANCE* inst, uint8_t dataset, const uint8_t peer[32], const char* ns) {
    ms_cancel_peer_scope(inst, dataset, peer, ns, 1);
}

void merkle_sync_cancel_ns(struct UTUN_INSTANCE* inst, const char* ns) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms || !ns) return;
    struct ms_session* s = ms->sessions;
    while (s) {
        if (!strcmp(s->ns, ns)) { ms_free(s); s = ms->sessions; }
        else s = s->next;
    }
}

void merkle_sync_cancel_peer(struct UTUN_INSTANCE* inst, const uint8_t peer[32]) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms) return;
    struct ms_session* s = ms->sessions;
    while (s) {
        if (peer && !memcmp(s->peer, peer, 32)) { ms_free(s); s = ms->sessions; }
        else s = s->next;
    }
}

void merkle_sync_cancel_dataset_ns(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms || !ns) return;
    struct ms_session* s = ms->sessions;
    while (s) {
        if (s->dataset->id == dataset && !strcmp(s->ns, ns)) { ms_free(s); s = ms->sessions; }
        else s = s->next;
    }
}

void merkle_sync_cancel_dataset_peer(struct UTUN_INSTANCE* inst, uint8_t dataset, const uint8_t peer[32]) {
    struct merkle_sync* ms = inst ? inst->msync : NULL;
    if (!ms) return;
    struct ms_session* s = ms->sessions;
    while (s) {
        if (s->dataset->id == dataset && peer && !memcmp(s->peer, peer, 32)) { ms_free(s); s = ms->sessions; }
        else s = s->next;
    }
}

void merkle_sync_recheck_peer(struct ETCP_CONN* conn) {
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    for (;;) {
        struct merkle_sync* ms = inst ? inst->msync : NULL;
        if (!ms) return;
        struct ms_session* selected = NULL;
        for (struct ms_session* s = ms->sessions; s; s = s->next)
            if (s->conn == conn && (s->state != MS_FAILED || s->restart) && s->state != MS_COMPLETE &&
                !ms_session_can_run(s)) { selected = s; break; }
        if (!selected) return;
        ms_fail(selected, MT_ERR_DISCONNECTED, "model policy changed", 0);
    }
}
