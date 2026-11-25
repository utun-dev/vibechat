#include "group_info.h"
#include "group_info_read.h"
#include "group_policy.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "../transport_layer/secure_channel.h"
#include "../utun_instance.h"
#include "../member_files/protocol.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include <stdio.h>
#include <string.h>

/* Первое поле job: только замороженные SQL-входы и собственный результат доступны worker. */
struct info_query {
    char channel[65];
    int editing;
    int writing;
    struct group_info_write write;
    char* result;
};
struct info_job {
    struct info_query sql;
    struct group_info_request* request;
    sqlite3* expected;
    uint64_t generation;
};

/* Короткий отказ не требует памяти даже после OOM work/submit; request/result освобождаются ровно здесь. */
static void info_reply(struct group_info_request* request, char* result) {
    const char* body = result ? result : "{\"ok\":false,\"error\":\"unavailable\"}";
    size_t bytes = strlen(body); uint8_t event[GROUP_INFO_MAX + 192];
    if (bytes > sizeof(event) - 40) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group settings response too large group=%s request=%llu bytes=%zu",
            request->group, (unsigned long long)request->id, bytes);
        body = "{\"ok\":false,\"error\":\"response_unavailable\"}"; bytes = strlen(body);
    }
    mf_put64(event, request->id); memset(event + 8, 0, 32);
    if (chat_channel_valid(request->group)) sc_hex_to_binary(request->group, event + 8, 32); memcpy(event + 40, body, bytes);
    chat_event_post(request->inst, CHAT_EVT_GROUP_INFO_RESULT, event, (int)bytes + 40); u_free(result); u_free(request);
}

static int info_read(sqlite3* db, void* arg) {
    struct info_query* query = arg;
    if (query->writing) {
        int code = group_info_command_write(db, &query->write); query->result = query->write.result; query->write.result = NULL;
        return code;
    }
    return query->editing ? SQLITE_OK : group_info_snapshot_read(db, query->channel, query->write.self_ed, &query->result);
}

static int info_current(void* data) {
    struct info_job* job = data; struct UTUN_INSTANCE* inst = job->request->inst; struct chat_core_ctx* cc = CC(inst);
    return !inst->destroying && cc && cc->initialized && (!db_async_can_write(inst->db_worker) || inst->group_started) &&
        cc->db == job->expected && inst->group_generation == job->generation;
}

/* Единственный прежний писатель остаётся в uasync; следующий READ увидит его commit после этого done. */
static void info_done(void* arg, const struct db_async_result* result) {
    struct info_job* job = arg; struct group_info_request* request = job->request;
    struct UTUN_INSTANCE* inst = request->inst; struct chat_core_ctx* cc = CC(inst);
    int ready = cc && cc->initialized, same_db = cc && cc->db == job->expected;
    int current = ready && same_db && (!db_async_can_write(inst->db_worker) || inst->group_started) &&
        inst->group_generation == job->generation, command_rc = 0;
    if (result->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group settings READ failed ch=%s request=%llu status=%d rc=%d: %s",
            job->sql.channel, (unsigned long long)request->id, result->status, result->sqlite_code, result->message);
        if (!job->sql.writing || !job->sql.write.status) { u_free(job->sql.result); job->sql.result = NULL; }
    } else if (!current) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group settings cancelled after service change ch=%s request=%llu generation=%llu"
            " current=%llu ready=%d same_db=%d",
            job->sql.channel, (unsigned long long)request->id, (unsigned long long)job->generation,
            (unsigned long long)inst->group_generation, ready, same_db);
        u_free(job->sql.result); job->sql.result = NULL;
    } else if (job->sql.writing) {
        if (job->sql.write.access) { group_policy_snapshot_publish(inst, job->sql.write.access); job->sql.write.access = NULL; }
        if (job->sql.write.changed) group_info_publish(inst, job->sql.channel);
    } else if (job->sql.editing) {
        command_rc = group_info_command(inst, job->sql.channel, request->name[0] ? request->name : NULL, NULL, request->clipboard_members - 1,
            request->expected[0] ? request->expected : NULL, &job->sql.result);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group settings result ch=%s request=%llu editing=%d sql_status=%d current=%d command_rc=%d bytes=%zu",
        job->sql.channel, (unsigned long long)request->id, job->sql.editing, result->status, current, command_rc,
        job->sql.result ? strlen(job->sql.result) : 0);
    char* response = job->sql.result; job->sql.result = NULL; info_reply(request, response); job->request = NULL;
}
static void info_cleanup(void* data) {
    struct info_job* job = data; group_policy_snapshot_free(job->sql.write.access); u_free(job->sql.result); u_free(job);
}

void group_info_gui_command(void* arg) {
    struct group_info_request* request = arg;
    if (!request || !request->inst) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group settings missing request/instance"); u_free(request); return;
    }
    struct UTUN_INSTANCE* inst = request->inst; struct chat_core_ctx* cc = CC(inst);
    int ready = cc && cc->initialized && (cc->db || db_async_can_write(inst->db_worker));
    int valid_group = chat_channel_valid(request->group);
    int name_terminated = memchr(request->name, 0, sizeof(request->name)) != NULL;
    int token_terminated = memchr(request->expected, 0, sizeof(request->expected)) != NULL;
    if (!ready || !valid_group || !name_terminated || !token_terminated ||
        request->clipboard_members < 0 || request->clipboard_members > 2) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group settings request refused group=%s request=%llu ready=%d valid_group=%d"
            " name_terminated=%d token_terminated=%d clipboard=%d", request->group,
            (unsigned long long)request->id, ready, valid_group, name_terminated, token_terminated, request->clipboard_members);
        info_reply(request, NULL); return;
    }
    struct info_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group settings request allocation failed group=%s request=%llu",
            request->group, (unsigned long long)request->id); info_reply(request, NULL); return;
    }
    job->request = request; job->expected = cc->db; job->generation = inst->group_generation;
    job->sql.editing = request->name[0] || request->clipboard_members;
    job->sql.writing = job->sql.editing && db_async_can_write(inst->db_worker);
    strcpy(job->sql.channel, request->group);
    job->sql.write = (struct group_info_write){.channel=job->sql.channel,
        .name=request->name[0] ? request->name : NULL, .expected=request->expected[0] ? request->expected : NULL,
        .clipboard_members=request->clipboard_members - 1};
    memcpy(job->sql.write.self_ed, inst->my_ed25519_pubkey, 32);
    if (inst->db_worker) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group settings READ queued ch=%s request=%llu editing=%d", job->sql.channel,
            (unsigned long long)request->id, job->sql.editing);
        db_async_run(inst->db_worker, &(struct db_async_task){.name="group-settings", .mode=job->sql.writing ? DB_ASYNC_WRITE : DB_ASYNC_READ,
            .guard=info_current, .work=info_read, .done=info_done, .cleanup=info_cleanup, .data=job}); return;
    }
    const char* file = sqlite3_db_filename(cc->db, "main");
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_MISUSE};
    if (file && !*file) {
        result.sqlite_code = info_read(cc->db, job); result.status = result.sqlite_code == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL;
        snprintf(result.message, sizeof(result.message), "%s", sqlite3_errstr(result.sqlite_code));
    } else snprintf(result.message, sizeof(result.message), "file database has no asynchronous reader");
    info_done(job, &result); info_cleanup(job);
}
