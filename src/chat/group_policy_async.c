#include "group_policy.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "../transport_layer/secure_channel.h"
#include "member_sync.h"
#include "merkle_sync.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../routing_layer/topo_group.h"
#include "../member_files/protocol.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>

/* Worker видит только первое поле job: самостоятельные входы и свой результат. */
struct policy_query {
    char channel[65];
    const char* keep;
    int all, editing;
    char* result;
    struct group_policy_mutation mutation;
    struct member_sync_write_context context;
};
struct policy_job {
    struct policy_query sql;
    struct group_policy_request* request;
    sqlite3* expected;
    uint64_t generation, core_generation, registration;
    int worker_write;
};

static void policy_reply(struct group_policy_request* request, char* result, const char* error, int committed) {
    uint8_t fallback[192]; uint8_t* event = fallback; size_t bytes; int copied = 0;
    if (result && result[0]) {
        bytes = strlen(result);
        if (bytes <= INT_MAX - 40) event = u_malloc(40 + bytes);
        else event = NULL;
        if (event) { memcpy(event + 40, result, bytes); copied = 1; }
        else {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group policy response allocation failed group=%s request=%llu bytes=%zu committed=%d",
                request->group, (unsigned long long)request->id, bytes, committed);
            event = fallback;
        }
    }
    if (!copied) {
        /* Даже при OOM не сообщаем отказ уже подтверждённой транзакции. Новый preview восстановит детали. */
        int size = committed ? snprintf((char*)fallback + 40, sizeof(fallback) - 40,
            "{\"ok\":true,\"applied\":true,\"all_members\":%s,\"can_edit\":true,\"details_unavailable\":true}",
            request->all_members ? "true" : "false") :
            snprintf((char*)fallback + 40, sizeof(fallback) - 40, "{\"ok\":false,\"error\":\"%s\"}", error);
        bytes = (size_t)size;
    }
    mf_put64(event, request->id); memset(event + 8, 0, 32);
    if (chat_channel_valid(request->group)) sc_hex_to_binary(request->group, event + 8, 32);
    chat_event_post(request->inst, CHAT_EVT_GROUP_POLICY_RESULT, event, (int)bytes + 40);
    if (event != fallback) u_free(event);
    u_free(result); u_free(request);
}

static int policy_read(sqlite3* db, void* arg) {
    struct policy_query* query = arg;
    if (query->editing && query->mutation.context) return group_policy_command_write(db, &query->mutation);
    return query->editing ? SQLITE_OK : group_policy_preview_read(db, query->channel, query->all, query->keep, &query->result);
}

static int policy_guard(void* data) {
    struct policy_job* job = data; struct UTUN_INSTANCE* inst = job->request->inst; struct chat_core_ctx* cc = CC(inst);
    if (inst->destroying || !cc || !cc->initialized || inst->group_generation != job->generation || cc->db != job->expected) return 0;
    if (!job->worker_write) return 1;
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, job->request->group);
    return inst->group_started && chat_core_generation(inst) == job->core_generation && group && !group->stopping &&
        group->generation == job->registration;
}

/* READ/done в общем FIFO: пока писатель legacy, правка выполняется здесь до следующего READ. */
static void policy_done(void* arg, const struct db_async_result* result) {
    struct policy_job* job = arg; struct group_policy_request* request = job->request;
    struct UTUN_INSTANCE* inst = request->inst; struct chat_core_ctx* cc = CC(inst);
    int ready = cc && cc->initialized, same_db = cc && cc->db == job->expected;
    int current = policy_guard(job), committed = 0, command_rc = 0;
    if (job->worker_write && job->sql.editing) { job->sql.result = job->sql.mutation.json; job->sql.mutation.json = NULL; }
    const char* error = "unavailable";
    if (result->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group policy READ failed ch=%s request=%llu status=%d rc=%d: %s", job->sql.channel,
            (unsigned long long)request->id, result->status, result->sqlite_code, result->message);
        if (result->sqlite_code == SQLITE_MISMATCH || result->sqlite_code == SQLITE_CONSTRAINT) error = "invalid_selection";
        u_free(job->sql.result); job->sql.result = NULL;
    } else if (!current) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group policy cancelled after service change ch=%s request=%llu generation=%llu"
            " current=%llu ready=%d same_db=%d", job->sql.channel, (unsigned long long)request->id,
            (unsigned long long)job->generation, (unsigned long long)inst->group_generation, ready, same_db);
        u_free(job->sql.result); job->sql.result = NULL;
    } else if (job->sql.editing && job->worker_write) {
        command_rc = job->sql.mutation.status; committed = !command_rc;
        if (committed) {
            struct group_policy_update* update = job->sql.mutation.admission; job->sql.mutation.admission = NULL;
            group_policy_publish(inst, update);
            if (policy_guard(job)) merkle_sync_changed(inst, MT_DATA_MEMBERS, job->sql.channel);
            DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "group policy committed ch=%s all=%d version=%llu", job->sql.channel,
                job->sql.all, (unsigned long long)job->sql.mutation.version);
        }
    } else if (job->sql.editing) {
        command_rc = group_policy_command(inst, job->sql.channel, job->sql.all, request->token, job->sql.keep, &job->sql.result);
        committed = command_rc == 0;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group policy result ch=%s request=%llu editing=%d sql_status=%d current=%d command_rc=%d bytes=%zu",
        job->sql.channel, (unsigned long long)request->id, job->sql.editing, result->status, current, command_rc,
        job->sql.result ? strlen(job->sql.result) : 0);
    char* response = job->sql.result; group_policy_discard(job->sql.mutation.admission);
    OPENSSL_cleanse(&job->sql.context, sizeof(job->sql.context)); u_free(job); policy_reply(request, response, error, committed);
}

void group_policy_gui_command(void* arg) {
    struct group_policy_request* request = arg;
    if (!request || !request->inst) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group policy missing request/instance"); u_free(request); return; }
    struct UTUN_INSTANCE* inst = request->inst; struct chat_core_ctx* cc = CC(inst);
    int worker_write = db_async_can_write(inst->db_worker);
    int ready = cc && cc->initialized && (worker_write ? inst->group_started : cc->db != NULL);
    int valid_group = chat_channel_valid(request->group);
    int terminated = memchr(request->token, 0, sizeof(request->token)) != NULL;
    if (!ready || !valid_group || !terminated || (request->all_members != 0 && request->all_members != 1)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group policy request refused group=%s request=%llu ready=%d valid_group=%d"
            " token_terminated=%d all=%d",
            request->group, (unsigned long long)request->id, ready, valid_group, terminated, request->all_members);
        policy_reply(request, NULL, "unavailable", 0); return;
    }
    struct policy_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group policy request allocation failed group=%s request=%llu",
            request->group, (unsigned long long)request->id); policy_reply(request, NULL, "unavailable", 0); return;
    }
    job->request = request; job->expected = cc->db; job->generation = inst->group_generation;
    job->sql.all = request->all_members; job->sql.keep = request->keep; job->sql.editing = request->token[0] != 0;
    job->worker_write = worker_write; job->core_generation = chat_core_generation(inst);
    struct TOPO_GROUP* topology = topo_groups_find_channel(inst->topo_groups, request->group);
    job->registration = topology ? topology->generation : 0;
    strcpy(job->sql.channel, request->group);
    if (worker_write && job->sql.editing) {
        job->sql.context.self = inst->node_id; job->sql.context.now = ntp_time_get_us(inst);

        memcpy(job->sql.context.public_key, inst->my_ed25519_pubkey, 32); memcpy(job->sql.context.private_key, inst->my_ed25519_privkey, 32);
        job->sql.mutation = (struct group_policy_mutation){.channel=job->sql.channel, .token=request->token,
            .keep=request->keep, .all_members=request->all_members, .context=&job->sql.context};
    }
    if (inst->db_worker) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group policy READ queued ch=%s request=%llu editing=%d", job->sql.channel,
            (unsigned long long)request->id, job->sql.editing);
        db_async_run(inst->db_worker, &(struct db_async_task){.name="group-policy",
            .mode=worker_write && job->sql.editing ? DB_ASYNC_WRITE : DB_ASYNC_READ,
            .guard=policy_guard, .work=policy_read, .done=policy_done, .data=job}); return;
    }
    const char* file = sqlite3_db_filename(cc->db, "main");
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_MISUSE};
    if (file && !*file) {
        result.sqlite_code = policy_read(cc->db, job); result.status = result.sqlite_code == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL;
        snprintf(result.message, sizeof(result.message), "%s", sqlite3_errstr(result.sqlite_code));
    } else snprintf(result.message, sizeof(result.message), "file database has no asynchronous reader");
    policy_done(job, &result);
}
