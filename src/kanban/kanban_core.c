#include "kanban_priv.h"
#include "../attachments/service.h"
#include "../attachments/download.h"
#include "kanban_read.h"
#include "../db_schema.h"
#include "../group/group_protocol.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp.h"
#include "../../lib/db_async.h"
#include "../../lib/json_flat.h"
#include <limits.h>

static int rollback(struct UTUN_INSTANCE* inst, const char* group) {
    if (sqlite3_get_autocommit(inst->topo_sqlite_db)) return 0;
    if (!kb_exec(inst->topo_sqlite_db, "ROLLBACK /* kanban */")) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "rollback failed ch=%s; requesting owner stop", group);
    utun_instance_stop(inst); return -1;
}

int kanban_prepare_group(struct UTUN_INSTANCE* inst, const char* group) {
    if (inst && db_async_can_write(inst->db_worker)) {
        if (group_protocol_current(inst, group) == GROUP_PROTOCOL_KANBAN) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board runtime not activated ch=%s", group ? group : "?"); return -1;
    }
    if (kb_type(inst ? inst->topo_sqlite_db : NULL, group)) return -1;
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "board prepared ch=%s service_started=%u", group, inst->kanban_started); return 0;
}

void kb_publish(struct UTUN_INSTANCE* inst, const char* group) {
    uint8_t event[32];
    if (!chat_channel_valid(group) || sc_hex_to_binary(group, event, sizeof(event))) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "invalid board address ch=%s", group ? group : "-"); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "board committed ch=%s", group);
    merkle_sync_changed(inst, MT_DATA_KANBAN, group);
    chat_event_post(inst, CHAT_EVT_KANBAN_CHANGED, event, sizeof(event));
}

/* Полный снимок ID отделён от SQLite перед callbacks, которые могут удалить группу. */
static int all_groups(struct UTUN_INSTANCE* inst, int rebuild) {
    char** rows = NULL; int count = 0, columns = 0; sqlite3* db = inst->topo_sqlite_db;
    if (sqlite3_get_table(db, "SELECT channel_id FROM channels WHERE group_type='kanban' ORDER BY channel_id",
        &rows, &count, &columns, NULL) != SQLITE_OK) return kb_error(db, NULL, "board catalog");
    int result = 0;
    for (int i = 0; i < count; i++) {
        const char* group = rows[(i + 1) * columns];
        if (rebuild) {
            if (kb_exec(db, "BEGIN IMMEDIATE /* kanban restore */") || kb_reindex(db, group, inst->my_ed25519_pubkey) ||
                kb_exec(db, "COMMIT /* kanban restore */")) { rollback(inst, group); result = -1; break; }
        } else kanban_members_changed(inst, group);
        struct TOPO_GROUP* topology = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, group) : NULL;
        for (struct ll_entry* e = topology && topology->senders_list ? topology->senders_list->head : NULL; e; e = e->next) {
            struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
            if (peer->conn) kanban_peer_ready(inst, group, peer->conn->peer_ed25519_pubkey);
        }
    }
    sqlite3_free_table(rows); return result;
}

int kanban_service_prepare(sqlite3* db, const uint8_t self[32]) {
    if (!db || !self || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board startup requires caller WRITE/database/self"); return SQLITE_MISUSE;
    }
    if (db_schema_table(db, "kanban_ops", NULL) || db_schema_table(db, "kanban_clock", NULL)) return SQLITE_ERROR;
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db,
        "SELECT channel_id FROM channels WHERE group_type='kanban' ORDER BY channel_id", -1, &st, NULL);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* group = (const char*)sqlite3_column_text(st, 0);
        if (!group) { rc = SQLITE_NOMEM; break; }
        if (!chat_channel_valid(group)) { rc = SQLITE_CORRUPT; break; }
        int indexed = kb_reindex(db, group, self);
        if (indexed < 0) { rc = -indexed; break; }
    }
    int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board SQL startup failed rc=%d finalize=%d: %s", rc, final, sqlite3_errstr(rc));
    else DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "board SQL startup prepared current state and Merkle; commit pending");
    return rc;
}

int kanban_service_activate(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->group_started || !inst->msync || inst->kanban_generation == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board activation requires common service/Merkle and available generation"); return -1;
    }
    if (inst->kanban_started) return 0;
    inst->kanban_generation++; inst->kanban_started = 1;
    if (kb_sync_init(inst)) { kanban_service_stop(inst); return -1; }
    for (struct ll_entry* entry = inst->topo_groups && inst->topo_groups->group_list ? inst->topo_groups->group_list->head : NULL;
        entry; entry = entry->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)entry;
        if (group->content_protocol != GROUP_PROTOCOL_KANBAN) continue;
        for (struct ll_entry* item = group->senders_list ? group->senders_list->head : NULL; item; item = item->next) {
            struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)item->data;
            if (peer->conn) kanban_peer_ready(inst, group->channel_id, peer->conn->peer_ed25519_pubkey);
        }
    }
    attachment_service_set_owner(inst, ATTACHMENT_OWNER_KANBAN, 1);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "kanban runtime activated generation=%llu", (unsigned long long)inst->kanban_generation); return 0;
}

int kanban_service_start(struct UTUN_INSTANCE* inst) {
    if (inst && db_async_can_write(inst->db_worker)) {
        if (group_service_start(inst)) return -1;
        return kanban_service_activate(inst);
    }
    if (!inst || group_service_start(inst) || !inst->msync || !sqlite3_get_autocommit(inst->topo_sqlite_db))
        return kb_error(inst ? inst->topo_sqlite_db : NULL, NULL, "kanban start requires common group service");
    if (inst->kanban_started) return 0;
    if (inst->kanban_generation == UINT64_MAX) return kb_error(inst->topo_sqlite_db, NULL, "kanban service generation exhausted");
    if (db_schema_table(inst->topo_sqlite_db, "kanban_ops", NULL) || db_schema_table(inst->topo_sqlite_db, "kanban_clock", NULL)) return -1;
    inst->kanban_generation++;
    inst->kanban_started = 1;
    if (kb_sync_init(inst) || all_groups(inst, 1)) { kanban_service_stop(inst); return -1; }
    attachment_service_set_owner(inst, ATTACHMENT_OWNER_KANBAN, 1);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "kanban service started generation=%llu", (unsigned long long)inst->kanban_generation); return 0;
}
void kanban_service_stop(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    attachment_service_set_owner(inst, ATTACHMENT_OWNER_KANBAN, 0);
    inst->kanban_started = 0; kb_sync_stop(inst);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "kanban service stopped; group membership and history retained");
}

struct kb_member_job {
    char group[65]; uint8_t self[32]; /* Frozen SQL-входы. */
    struct UTUN_INSTANCE* inst; uint64_t generation; /* Только guard/done в uasync. */
};
static int kb_member_current(void* data) {
    struct kb_member_job* job = data;
    return !job->inst->destroying && job->inst->group_started &&
        chat_core_is_initialized(job->inst) && chat_core_generation(job->inst) == job->generation &&
        group_protocol_current(job->inst, job->group) == GROUP_PROTOCOL_KANBAN;
}
static int kb_member_work(sqlite3* db, void* data) {
    struct kb_member_job* job = data; int rc = kb_reindex(db, job->group, job->self); return rc < 0 ? -rc : SQLITE_OK;
}
static void kb_member_done(void* data, const struct db_async_result* result) {
    struct kb_member_job* job = data;
    if (result->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board membership projection failed ch=%s status=%d rc=%d: %s",
            job->group, result->status, result->sqlite_code, result->message); return;
    }
    if (!kb_member_current(job)) return;
    merkle_sync_revalidate_peers(job->inst, MT_DATA_KANBAN, job->group);
    if (kb_member_current(job) && job->inst->kanban_started) kb_publish(job->inst, job->group);
}
static void kb_member_free(void* data) { u_free(data); }

void kanban_members_changed(struct UTUN_INSTANCE* inst, const char* group) {
    if (inst && db_async_can_write(inst->db_worker)) {
        if (!chat_channel_valid(group)) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board membership invalid group"); return; }
        struct kb_member_job* job = u_calloc(1, sizeof(*job));
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "board membership allocation ch=%s", group); return; }
        strcpy(job->group, group); memcpy(job->self, inst->my_ed25519_pubkey, 32); job->inst = inst; job->generation = chat_core_generation(inst);
        db_async_run(inst->db_worker, &(struct db_async_task){.name="kanban-members", .mode=DB_ASYNC_WRITE,
            .guard=kb_member_current, .work=kb_member_work, .done=kb_member_done, .cleanup=kb_member_free, .data=job}); return;
    }
    if (!inst || !inst->kanban_started) return;
    if (kb_exec(inst->topo_sqlite_db, "BEGIN IMMEDIATE /* kanban membership */") ||
        kb_reindex(inst->topo_sqlite_db, group, inst->my_ed25519_pubkey) ||
        kb_exec(inst->topo_sqlite_db, "COMMIT /* kanban membership */")) { rollback(inst, group); return; }
    merkle_sync_revalidate_peers(inst, MT_DATA_KANBAN, group); kb_publish(inst, group);
}

/* Прежний writer временно вызывает тот же SQL work; публикация только после собственного COMMIT. */
int kanban_command(struct UTUN_INSTANCE* inst, const char* group, const char* json, char** result) {
    if (result) *result = NULL;
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!result || !inst || !inst->kanban_started || !db || !chat_channel_valid(group) || !json)
        return kb_error(db, group, "command service/arguments");
    if (!sqlite3_get_autocommit(db)) return kb_error(db, group, "command requires own transaction");
    struct kanban_write query = {.group=group, .json=json, .writer={.now=ntp_time_get_us(inst)}};
    memcpy(query.writer.public_key, inst->my_ed25519_pubkey, 32);
    memcpy(query.writer.private_key, inst->my_ed25519_privkey, 32);
    int rc = kb_exec(db, "BEGIN IMMEDIATE /* kanban command */");
    if (!rc) rc = kanban_command_write(db, &query);
    if (!rc && kb_exec(db, "COMMIT /* kanban command */")) { rc = -1; query.error = "database_error"; }
    OPENSSL_cleanse(&query.writer, sizeof(query.writer));
    if (rc) {
        rollback(inst, group); u_free(query.result); query.result = NULL;
        struct strbuf response = {0}; const char* reason = query.error ? query.error : "database_error";
        if (strbuf_addf(&response, "{\"ok\":false,\"error\":\"%s\"}", reason) >= 0) query.result = strbuf_detach(&response);
        strbuf_free(&response);
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "command rejected ch=%s reason=%s rc=%d", group, reason, rc);
    }
    *result = query.result;
    if (!rc) kb_publish(inst, group);
    return rc ? -1 : 0;
}

/* Отзыв уже находится внутри транзакции MEMBERS; worker-совместимая часть не получает instance. */
int kanban_seal_member(struct UTUN_INSTANCE* inst, const char* group, const uint8_t member[32],
    const uint8_t generation[32], char seal[65]) {
    if (!inst) return kb_error(NULL, group, "seal missing instance");
    struct kanban_identity writer = {.now=ntp_time_get_us(inst)};
    memcpy(writer.public_key, inst->my_ed25519_pubkey, 32); memcpy(writer.private_key, inst->my_ed25519_privkey, 32);
    int rc = kanban_seal_write(inst->topo_sqlite_db, &writer, group, member, generation, seal);
    OPENSSL_cleanse(&writer, sizeof(writer)); return rc;
}

struct kb_gui_query {
    char group[65]; /* Frozen canonical group; SQL не обращается к instance. */
    const char* json; /* Неизменяемая часть owned kanban_request до done. */
    int command; /* SQL классифицировал изменение, которое пока исполняется прежним писателем в uasync. */
    int write_in_worker, changed; /* RW backend выполняет изменение целиком; changed публикуется только после commit. */
    struct kanban_identity writer; /* Frozen копия для WRITE; очищается в done, worker не получает instance. */
    const char* error; /* Статическая причина SQL work; используется после rollback для короткого отказа. */
    char* result; /* Собственный JSON worker либо результат изменения, u_free в reply. */
};
struct kb_gui_job {
    struct kb_gui_query query; /* Первое поле: worker получает только этот SQL-контекст. */
    struct kanban_request* request; /* Только uasync; владеет исходным JSON/корреляцией. */
    sqlite3* expected; /* Только uasync: привязка к исходной БД без использования handle в worker. */
    uint64_t generation; /* Только uasync: запуск канбана, принявший исходную команду. */
    uint64_t core_generation, registration; /* RW namespace lifetime; одинаковый group ID после remove не подходит. */
    kanban_execute_done_fn done;
    void* arg;
};

/* Даже при OOM большого ответа короткий отказ формируется на стеке; caller больше не владеет req/result. */
static void kb_gui_reply(struct kanban_request* req, const char* result) {
    const char* body = result ? result : "{\"ok\":false,\"error\":\"unavailable\"}";
    size_t bytes = strlen(body); uint8_t fallback[128]; uint8_t* event = fallback;
    if (bytes > sizeof(fallback) - 40) {
        event = bytes <= INT_MAX - 40 ? u_malloc(bytes + 40) : NULL;
        if (!event) {
            DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "UI response allocation/size failure group=%s request=%llu bytes=%zu",
                req->group, (unsigned long long)req->request, bytes);
            body = "{\"ok\":false,\"error\":\"response_unavailable\"}"; bytes = strlen(body); event = fallback;
        }
    }
    for (unsigned i = 0; i < 8; i++) event[i] = (uint8_t)(req->request >> (i * 8));
    memset(event + 8, 0, 32);
    if (chat_channel_valid(req->group)) sc_hex_to_binary(req->group, event + 8, 32);
    memcpy(event + 40, body, bytes); chat_event_post(req->inst, CHAT_EVT_KANBAN_RESULT, event, (int)bytes + 40);
    if (event != fallback) u_free(event);
    u_free(req);
}

/* Классификация JSON и чтение materialized-доски/истории — одна READ, без runtime и сетевых callbacks. */
static int kb_gui_read_work(sqlite3* db, void* data) {
    struct kb_gui_query* query = data; sqlite3_stmt* st = NULL; char entity[65] = {0};
    int rc = sqlite3_prepare_v2(db, "SELECT CASE WHEN json_valid(?1) THEN json_extract(?1,'$.action') END", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, query->json, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    const char* action = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
    if (rc == SQLITE_ROW && !action && sqlite3_errcode(db) == SQLITE_NOMEM) rc = SQLITE_NOMEM;
    int snapshot = action && !strcmp(action, "snapshot");
    int history = action && !strcmp(action, "history");
    int messages = action && !strcmp(action, "messages");
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK) {
        int error = final == SQLITE_OK ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "UI classify READ failed ch=%s rc=%d finalize=%d: %s", query->group, rc, final, sqlite3_errmsg(db));
        return error;
    }
    if (!snapshot && !history && !messages) {
        if (!query->write_in_worker) { query->command = 1; return SQLITE_OK; }
        struct kanban_write change = {.group=query->group, .json=query->json, .writer=query->writer};
        rc = kanban_command_write(db, &change); query->result = change.result; query->error = change.error;
        query->changed = rc == SQLITE_OK; OPENSSL_cleanse(&change.writer, sizeof(change.writer)); return rc;
    }
    if (history || messages) {
        st = NULL;
        rc = sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.entity')", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, query->json, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const char* value = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0);
            if (!value && sqlite3_errcode(db) == SQLITE_NOMEM) rc = SQLITE_NOMEM;
            else if (!value || bytes != 64 || memchr(value, 0, 64)) rc = SQLITE_MISMATCH;
            else { memcpy(entity, value, 64); rc = SQLITE_OK; }
        }
        final = sqlite3_finalize(st);
        if (rc != SQLITE_OK || final != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "UI history entity READ failed ch=%s rc=%d finalize=%d: %s", query->group, rc, final, sqlite3_errmsg(db));
            return final == SQLITE_OK ? rc : final;
        }
    }
    char* board = NULL;
    rc = messages ? kanban_messages_read(db, query->group, query->json, &board) :
        history ? kanban_history_read(db, query->group, entity, &board) : kanban_snapshot_read(db, query->group, &board);
    if (rc != SQLITE_OK) return rc;
    struct strbuf json = {0};
    int added = messages ? strbuf_addf(&json, "{\"ok\":true,\"entity\":\"%s\",%s", entity, board + 1) :
        strbuf_addf(&json, "{\"ok\":true,\"entity\":\"%s\",\"%s\":%s}", entity, history ? "history" : "board", board);
    if (added >= 0) query->result = strbuf_detach(&json);
    strbuf_free(&json); u_free(board);
    if (!query->result) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "UI READ serialization allocation failed ch=%s history=%d", query->group, history); return SQLITE_NOMEM;
    }
    return SQLITE_OK;
}

/* Uasync-проверка перед dispatch и после завершения: поздняя команда не принадлежит новому запуску. */
static int kb_gui_current(void* data) {
    struct kb_gui_job* job = data; struct UTUN_INSTANCE* inst = job->request->inst;
    if (inst->destroying || !inst->kanban_started || inst->kanban_generation != job->generation || inst->topo_sqlite_db != job->expected) return 0;
    if (!job->query.write_in_worker) return 1;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, job->request->group) : NULL;
    return inst->group_started && job->core_generation && chat_core_generation(inst) == job->core_generation &&
        group && !group->stopping && group->generation == job->registration;
}

/* Только uasync: READ завершена; прежний писатель исполняет изменение и публикует эффекты в своём потоке. */
static void kb_gui_read_done(void* data, const struct db_async_result* status) {
    struct kb_gui_job* job = data; struct kb_gui_query* query = &job->query; struct kanban_request* req = job->request;
    int current = kb_gui_current(job), command_status = 0;
    if (status->status != DB_ASYNC_OK) {
        if (status->status == DB_ASYNC_CANCELLED) DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN,
            "UI request cancelled before SQL ch=%s request=%llu generation=%llu", query->group,
            (unsigned long long)req->request, (unsigned long long)job->generation);
        else DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "UI READ failed ch=%s request=%llu status=%d rc=%d: %s",
                query->group, (unsigned long long)req->request, status->status, status->sqlite_code, status->message);
        u_free(query->result); query->result = NULL;
        if (query->error && current) {
            struct strbuf error = {0};
            if (strbuf_addf(&error, "{\"ok\":false,\"error\":\"%s\"}", query->error) >= 0) query->result = strbuf_detach(&error);
            strbuf_free(&error);
        }
    } else if (query->changed) {
        if (current) kb_publish(req->inst, query->group);
        else DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "accepted WRITE committed after service change ch=%s request=%llu; publication skipped",
            query->group, (unsigned long long)req->request);
    } else if (!current) {
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "UI command cancelled after service change ch=%s request=%llu generation=%llu current=%llu started=%u",
            query->group, (unsigned long long)req->request, (unsigned long long)job->generation,
            (unsigned long long)req->inst->kanban_generation, req->inst->kanban_started);
        u_free(query->result); query->result = NULL;
    } else if (query->command) command_status = kanban_command(req->inst, query->group, req->json, &query->result);
    struct kanban_execute_result result = {.status=status->status || command_status || (!current && !query->changed) ? -1 : 0,
        .sqlite_code=status->sqlite_code ? status->sqlite_code : !current && !query->changed ? SQLITE_ABORT : SQLITE_OK,
        .committed=status->status == DB_ASYNC_OK && !command_status && (query->changed || (current && query->command)),
        .json=query->result ? query->result : "{\"ok\":false,\"error\":\"unavailable\"}"};
    job->done(job->arg, &result);
    OPENSSL_cleanse(&query->writer, sizeof(query->writer)); u_free(query->result); u_free(req); u_free(job);
}

void kanban_execute_submit(struct UTUN_INSTANCE* inst, const char* group, const char* json, kanban_execute_done_fn done, void* arg) {
    struct kanban_execute_result failure = {.status=-1,.sqlite_code=SQLITE_MISUSE,.json="{\"ok\":false,\"error\":\"unavailable\"}"};
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "kanban execute requires completion"); return; }
    if (!inst || !inst->kanban_started || !chat_channel_valid(group) || !json || strlen(json) > 16384) {
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "kanban execute invalid/stopped input"); done(arg, &failure); return;
    }
    struct kanban_request* req = u_calloc(1, sizeof(*req) + strlen(json) + 1);
    struct kb_gui_job* job = u_calloc(1, sizeof(*job));
    if (!req || !job) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "kanban execute allocation failed"); u_free(req); u_free(job); failure.sqlite_code = SQLITE_NOMEM; done(arg, &failure); return; }
    req->inst = inst; strcpy(req->group, group); strcpy(req->json, json); job->done = done; job->arg = arg;
    job->request = req; job->expected = req->inst->topo_sqlite_db; job->query.json = req->json;
    job->generation = req->inst->kanban_generation;
    job->core_generation = chat_core_generation(inst);
    struct TOPO_GROUP* topology = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, req->group) : NULL;
    job->registration = topology ? topology->generation : 0;
    strcpy(job->query.group, group);
    if (req->inst->db_worker) {
        job->query.write_in_worker = db_async_can_write(req->inst->db_worker);
        char action[32]; int reading = !json_flat_get(req->json, "action", action, sizeof(action)) &&
            (!strcmp(action, "snapshot") || !strcmp(action, "history") || !strcmp(action, "messages"));
        enum db_async_mode mode = job->query.write_in_worker && !reading ? DB_ASYNC_WRITE : DB_ASYNC_READ;
        if (mode == DB_ASYNC_WRITE) {
            job->query.writer.now = ntp_time_get_us(req->inst);
            memcpy(job->query.writer.public_key, req->inst->my_ed25519_pubkey, 32);
            memcpy(job->query.writer.private_key, req->inst->my_ed25519_privkey, 32);
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "UI %s queued ch=%s request=%llu worker_write=%d",
            mode == DB_ASYNC_WRITE ? "WRITE" : "READ", job->query.group, (unsigned long long)req->request, job->query.write_in_worker);
        db_async_submit_guarded(req->inst->db_worker, "kanban-ui", mode,
            kb_gui_current, kb_gui_read_work, job, kb_gui_read_done); return;
    }
    const char* path = job->expected ? sqlite3_db_filename(job->expected, "main") : NULL;
    struct db_async_result result = {DB_ASYNC_CLOSED, SQLITE_MISUSE, "file database reader unavailable"};
    if (path && !*path) {
        result.sqlite_code = kb_gui_read_work(job->expected, &job->query);
        result.status = result.sqlite_code == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL;
        snprintf(result.message, sizeof(result.message), "%s", sqlite3_errstr(result.sqlite_code));
    } else DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "UI READ: asynchronous file database reader unavailable ch=%s", job->query.group);
    kb_gui_read_done(job, &result);
}
static void kb_gui_completed(void* data, const struct kanban_execute_result* result) {
    kb_gui_reply(data, result->json);
}

struct kb_gui_download {
    struct kanban_request* request; /* Владеет корреляцией UI до единственного ответа. */
    uint64_t generation, core_generation, registration; /* Запуск сервиса и конкретная регистрация группы. */
};

/* Общая загрузка уже проверила ссылку, допуск и файл; поздний ответ не принадлежит новому запуску/группе. */
static void kb_gui_download_done(void* data, int error, const char* path) {
    struct kb_gui_download* download = data; struct kanban_request* req = download->request;
    struct UTUN_INSTANCE* inst = req->inst;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, req->group) : NULL;
    int current = !inst->destroying && inst->kanban_started && inst->kanban_generation == download->generation &&
        chat_core_generation(inst) == download->core_generation && group && !group->stopping && group->generation == download->registration;
    struct strbuf json = {0}; int success = !error && current && path && *path;
    if (success) {
        if (strbuf_addf(&json, "{\"ok\":true,\"path\":") < 0 || strbuf_add_json_string(&json, path) || strbuf_addf(&json, "}") < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card download response allocation failed group=%s", req->group);
            success = 0;
        }
    }
    if (!success) DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "card download failed group=%s request=%llu error=%d current=%d",
        req->group, (unsigned long long)req->request, error, current);
    else DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "card download complete group=%s request=%llu path=%s",
        req->group, (unsigned long long)req->request, path);
    if (current) {
        const char* channel = req->group;
        uint8_t event[32];
        if (!sc_hex_to_binary(channel, event, sizeof(event))) chat_event_post(inst, CHAT_EVT_KANBAN_CHANGED, event, sizeof(event));
    }
    kb_gui_reply(req, success ? strbuf_str(&json) : "{\"ok\":false,\"error\":\"download_failed\"}");
    strbuf_free(&json); u_free(download);
}

/* UI передаёт постоянный ID сообщения и UUID объекта; manifest/путь/права читаются общим backend из БД. */
static void kb_gui_download_start(struct kanban_request* req) {
    char entity[66], object[34]; struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_KANBAN};
    struct UTUN_INSTANCE* inst = req->inst;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, req->group) : NULL;
    const char* group_id = group ? group->channel_id : "";
    if (!inst->kanban_started || !group || group->stopping || group_protocol_current(inst, group_id) != GROUP_PROTOCOL_KANBAN ||
        json_flat_get(req->json, "entity", entity, sizeof(entity)) || kb_unhex(entity, ref.message) ||
        json_flat_get(req->json, "object", object, sizeof(object)) || strlen(object) != 32 ||
        strspn(object, "0123456789abcdefABCDEF") != 32 || sc_hex_to_binary(object, ref.object, sizeof(ref.object))) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card download invalid/stopped group=%s request=%llu",
            req->group, (unsigned long long)req->request);
        kb_gui_reply(req, "{\"ok\":false,\"error\":\"invalid_download\"}"); return;
    }
    struct kb_gui_download* download = u_calloc(1, sizeof(*download));
    if (!download) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card download allocation failed group=%s", group_id); kb_gui_reply(req, NULL); return;
    }
    download->request = req; download->generation = inst->kanban_generation;
    download->core_generation = chat_core_generation(inst); download->registration = group->generation;
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "card download started group=%s message=%s object=%s request=%llu",
        group_id, entity, object, (unsigned long long)req->request);
    strcpy(ref.space, group_id);
    attachment_group_download(inst, &ref, kb_gui_download_done, download);
}

void kanban_gui_command(void* arg) {
    struct kanban_request* req = arg;
    if (!req || !req->inst) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "UI command missing request/instance"); u_free(req); return; }
    char action[32];
    if (!json_flat_get(req->json, "action", action, sizeof(action)) && !strcmp(action, "download")) {
        kb_gui_download_start(req); return;
    }
    const char* group = req->group;
    kanban_execute_submit(req->inst, group, req->json, kb_gui_completed, req);
}
