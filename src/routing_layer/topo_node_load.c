#include "topo_node_load.h"
#include "../utun_instance.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <stdio.h>

/* Только этот вложенный объект передаётся worker; runtime/callback остаются в оболочке uasync. */
struct node_query {
    uint64_t node_id;                       /* Идентичность замороженного запроса. */
    uint8_t x[SC_PUBKEY_SIZE]; int keyed; /* Копия полного ключа на время запроса. */
    enum topo_node_record_kind kind;       /* Одна выборка за работу READ. */
    struct topo_node_record* record;       /* Собственный результат worker до передачи в done. */
};
struct topo_node_load {
    struct node_query query;               /* Вход/выход SQL, uasync не трогает до read_done. */
    struct UTUN_INSTANCE* inst;            /* Только uasync; NULL после detach/cancel/stop. */
    struct topo_node_load* next;           /* Реестр запросов instance, только uasync. */
    struct posted_task* posted;            /* Начальный callback, зарезервирован до принятия. */
    topo_node_load_done done;              /* Потребитель; NULL после отмены/завершения. */
    void* data;                           /* Заимствован только uasync до callback/cancel. */
    enum topo_node_record_kind fallback;  /* Вторая выборка либо SNAPSHOT для единственной. */
    int sql_pending, finishing;            /* Владение SQL-результатом и защита cancel из done. */
};

static void read_done(void* data, const struct db_async_result* result);

static void detach(struct topo_node_load* request) {
    if (!request->inst) return;
    struct topo_node_load** link = (struct topo_node_load**)&request->inst->node_loads;
    while (*link && *link != request) link = &(*link)->next;
    if (*link) *link = request->next;
    request->next = NULL; request->inst = NULL;
}
static void dispose(struct topo_node_load* request) {
    topo_node_sqlite_record_free(request->query.record); u_free(request);
}

/* Registry ref удерживается даже при callbacks, меняющих состав/состояние группы. */
static void finish(struct topo_node_load* request, struct TOPO_NODE* node, const struct db_async_result* result) {
    struct UTUN_INSTANCE* inst = request->inst;
    struct TOPO_GROUPS* groups = inst ? inst->topo_groups : NULL;
    topo_node_load_done done = request->done; void* data = request->data;
    detach(request); request->done = NULL; request->data = NULL; request->finishing = 1;
    if (done) {
        if (inst) inst->node_load_callbacks++;
        done(data, node, result);
        if (inst) inst->node_load_callbacks--;
    }
    if (node) topo_node_registry_release(groups, node);
    request->finishing = 0;
    if (!request->sql_pending) dispose(request);
}

static int read_work(sqlite3* db, void* data) {
    struct node_query* query = data;
    return topo_node_sqlite_record_lookup(db, query->node_id, query->keyed ? query->x : NULL, query->kind, &query->record);
}
static void submit_read(struct topo_node_load* request) {
    request->sql_pending = 1;
    if (request->inst->db_worker) {
        db_async_submit(request->inst->db_worker, "load-node", DB_ASYNC_READ, read_work, &request->query, read_done); return;
    }
    struct db_async_result result = {0};
    sqlite3* db = request->inst->topo_sqlite_db;
    const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    if (db && (!file || !*file)) {
        result.sqlite_code = read_work(db, &request->query);
        if (result.sqlite_code) result.status = DB_ASYNC_SQL;
        snprintf(result.message, sizeof(result.message), "%s", sqlite3_errstr(result.sqlite_code));
    } else {
        result.status = DB_ASYNC_CLOSED; result.sqlite_code = SQLITE_MISUSE;
        snprintf(result.message, sizeof(result.message), "persistent database worker is unavailable");
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node load rejected: persistent database requires worker node=%016llx",
            (unsigned long long)request->query.node_id);
    }
    read_done(&request->query, &result);
}
static void read_done(void* data, const struct db_async_result* result) {
    struct topo_node_load* request = data; /* query — первое поле, worker не получает остальные поля. */
    request->sql_pending = 0;
    if (request->finishing) return; /* Вложенный poll из stop callback: dispose выполнит внешний finish. */
    if (!request->inst) { dispose(request); return; }
    if (result->status || request->inst->destroying) {
        struct db_async_result error = *result;
        if (!error.status) { error.status = DB_ASYNC_CLOSED; snprintf(error.message, sizeof(error.message), "instance is stopping"); }
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "node load failed node=%016llx status=%d rc=%d: %s",
            (unsigned long long)request->query.node_id, error.status, error.sqlite_code, error.message);
        finish(request, NULL, &error); return;
    }
    if (!request->query.record) {
        struct TOPO_NODE* current = topo_node_registry_lookup(request->inst->topo_groups, request->query.node_id, request->query.keyed ? request->query.x : NULL);
        if (current) {
            topo_node_ref(current); finish(request, current, result); return;
        }
    }
    if (!request->query.record && request->query.kind == TOPO_NODE_RECORD_SNAPSHOT && request->fallback != TOPO_NODE_RECORD_SNAPSHOT) {
        request->query.kind = request->fallback; submit_read(request); return;
    }
    struct TOPO_NODE* node = topo_node_sqlite_record_build(request->inst->topo_groups, request->query.record);
    if (request->query.record && !node) {
        struct db_async_result error = {.status=DB_ASYNC_SQL, .sqlite_code=SQLITE_ERROR};
        snprintf(error.message, sizeof(error.message), "stored node validation/allocation failed");
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node load build failed node=%016llx kind=%d",
            (unsigned long long)request->query.node_id, request->query.kind);
        finish(request, NULL, &error); return;
    }
    if (node) {
        struct TOPO_NODE* stored = topo_node_registry_put(request->inst->topo_groups, node);
        if (!stored) {
            topo_node_destroy(request->inst->topo_groups, node);
            struct db_async_result error = {.status=DB_ASYNC_NOMEM, .sqlite_code=SQLITE_NOMEM};
            snprintf(error.message, sizeof(error.message), "node registry allocation failed");
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node load registry failed node=%016llx", (unsigned long long)request->query.node_id);
            finish(request, NULL, &error); return;
        }
        node = stored;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node load completed node=%016llx found=%d kind=%d",
        (unsigned long long)request->query.node_id, node != NULL, request->query.kind);
    finish(request, node, result);
}

static void start(void* data) {
    struct topo_node_load* request = data; request->posted = NULL;
    if (!request->inst) { request->sql_pending = 0; if (!request->finishing) dispose(request); return; }
    if (request->inst->destroying || request->inst->node_load_stopping) {
        const struct db_async_result error = {.status=DB_ASYNC_CLOSED, .message="instance is stopping"};
        finish(request, NULL, &error); return;
    }
    struct TOPO_NODE* existing = topo_node_registry_lookup(request->inst->topo_groups, request->query.node_id, request->query.keyed ? request->query.x : NULL);
    if (existing && !request->inst->destroying) {
        struct db_async_result result = {0}; topo_node_ref(existing);
        finish(request, existing, &result); return;
    }
    submit_read(request);
}

struct topo_node_load* topo_node_load_async(struct UTUN_INSTANCE* inst, uint64_t node_id,
    enum topo_node_record_kind fallback, topo_node_load_done done, void* data) {
    return topo_node_load_key_async(inst, node_id, NULL, fallback, done, data);
}

struct topo_node_load* topo_node_load_key_async(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x,
    enum topo_node_record_kind fallback, topo_node_load_done done, void* data) {
    if (!inst || inst->destroying || inst->node_load_stopping || !inst->ua || !inst->topo_groups || !node_id || !done ||
        fallback < TOPO_NODE_RECORD_SNAPSHOT || fallback > TOPO_NODE_RECORD_CONNECT) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node load invalid arguments node=%016llx fallback=%d", (unsigned long long)node_id, fallback); return NULL;
    }
    struct topo_node_load* request = u_calloc(1, sizeof(*request));
    struct posted_task* posted = request ? u_calloc(1, sizeof(*posted)) : NULL;
    if (!request || !posted) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node load reserve failed node=%016llx request=%d completion=%d",
            (unsigned long long)node_id, !!request, !!posted); u_free(request); return NULL;
    }
    if (x) { memcpy(request->query.x, x, sizeof(request->query.x)); request->query.keyed = 1; }
    request->query.node_id = node_id; request->query.kind = TOPO_NODE_RECORD_SNAPSHOT; request->fallback = fallback;
    request->inst = inst; request->done = done; request->data = data; request->posted = posted;
    request->next = inst->node_loads; inst->node_loads = request;
    posted->callback = start; posted->arg = request; uasync_post_reserved(inst->ua, posted);
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node load accepted node=%016llx fallback=%d", (unsigned long long)node_id, fallback);
    return request;
}

void topo_node_load_cancel(struct topo_node_load* request) {
    if (!request || request->finishing) return;
    struct UASYNC* ua = request->inst ? request->inst->ua : NULL;
    detach(request); request->done = NULL; request->data = NULL;
    if (request->posted) {
        struct posted_task* posted = request->posted; request->posted = NULL;
        if (uasync_cancel_post(ua, posted) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node load cancel could not remove reserved start node=%016llx",
                (unsigned long long)request->query.node_id); return;
        }
    }
    if (!request->sql_pending) dispose(request);
}

unsigned topo_node_load_stop(struct UTUN_INSTANCE* inst) {
    if (!inst) return 0;
    inst->node_load_stopping = 1;
    unsigned count = 0;
    const struct db_async_result error = {.status=DB_ASYNC_CLOSED, .message="instance is stopping"};
    while (inst->node_loads) {
        struct topo_node_load* request = inst->node_loads;
        if (request->posted) {
            struct posted_task* posted = request->posted; request->posted = NULL;
            if (uasync_cancel_post(inst->ua, posted) != ERR_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node load stop could not remove reserved start node=%016llx",
                    (unsigned long long)request->query.node_id); request->sql_pending = 1;
            }
        }
        count++; finish(request, NULL, &error);
    }
    if (count) DEBUG_INFO(DEBUG_CATEGORY_BGP, "node loads stopped count=%u; pending SQL results will only free their data", count);
    return count;
}
