#include "group_protocol.h"
#include "../chat/chat_core_priv.h"
#include "../chat/chat_feedback.h"
#include "../chat/chat_log.h"
#include "../chat/group_info.h"
#include "../chat/group_policy.h"
#include "../chat/member_sync.h"
#include "../chat/merkle_sync.h"
#include "../kanban/kanban.h"
#include "../kanban/kanban_sql.h"
#include "../ntp_time.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../../lib/mem.h"
#include "../../lib/db_async.h"
#include <limits.h>

int group_protocol_type(sqlite3* db, const char* group) {
    struct group_info_record info; struct group_info_profile profile;
    int rc = group_info_get(db, group, &info); if (!rc) rc = group_info_parse(db, info.json, &profile);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group protocol unavailable ch=%s rc=%d", group ? group : "?", rc); return rc;
    }
    return !strcmp(profile.group_type, "chat") ? 1 : !strcmp(profile.group_type, "kanban") ? 2 : 0;
}

enum group_protocol_kind group_protocol_current(struct UTUN_INSTANCE* inst, const char* group) {
    if (!inst || !chat_channel_valid(group)) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group protocol runtime: invalid instance/group"); return GROUP_PROTOCOL_UNKNOWN; }
    struct TOPO_GROUP* topology = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, group) : NULL;
    return topology && !topology->stopping && topology->group_type == TOPO_GROUP_TYPE_CHAT
        ? (enum group_protocol_kind)topology->content_protocol : GROUP_PROTOCOL_UNKNOWN;
}
int group_protocol_schema(struct UTUN_INSTANCE* inst, const char* group) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "legacy group schema rejected in RW; use prepare_write"); return -1;
    }
    int type = group_protocol_type(inst ? inst->topo_sqlite_db : NULL, group);
    return type < 0 ? -1 : type == 1 && inst->chat_content_started ? chat_feedback_ensure_channel(inst->topo_sqlite_db, group) : 0;
}

static void content_ready(struct TOPO_GROUP* group, const uint8_t peer[32], int ready, void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    if (ready) group_protocol_peer_ready(inst, group->channel_id, peer);
    else {
        merkle_sync_cancel(inst, MT_DATA_KANBAN, peer, group->channel_id);
        merkle_sync_cancel(inst, MT_DATA_CHAT_LOG, peer, group->channel_id);
    }
}

int group_protocol_prepare_write(sqlite3* db, const char* group, int chat_content, struct group_protocol_prepared* out) {
    if (!db || !chat_channel_valid(group) || !out || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group SQL preparation requires channel/output/caller WRITE"); return SQLITE_MISUSE;
    }
    struct group_protocol_prepared result = {0}; int type = group_protocol_type(db, group);
    if (type < 0) return -type;
    int code = topo_node_sqlite_member_local_init_write(db, group); if (code) return code;
    result.kind = (enum group_protocol_kind)type;
    if (type == GROUP_PROTOCOL_CHAT && chat_content) {
        code = chat_feedback_ensure_channel(db, group); if (code) return -code;
        char table[96]; snprintf(table, sizeof(table), "msg_%s", group);
        int rc = db_sync_instance_prepare(db, table, group, &result.history); if (rc) return rc;
    }
    *out = result; return SQLITE_OK;
}

int group_protocol_activate(struct UTUN_INSTANCE* inst, const char* group, const struct group_protocol_prepared* prepared) {
    if (!inst || !prepared || !chat_channel_valid(group)) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group activation invalid context/channel"); return -1; }
    int type = prepared->kind;
    struct TOPO_GROUP* topology = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, group) : NULL;
    if (!topology || topology->stopping || topology->group_type != TOPO_GROUP_TYPE_CHAT || type < 0 || type > GROUP_PROTOCOL_KANBAN) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group activation requires live CHAT topology ch=%s kind=%d", group, type); return -1;
    }
    if (topology) {
        if (topology->content_protocol && topology->content_protocol != type) {
            DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "prepared group protocol conflict ch=%s previous=%u incoming=%d",
                group, topology->content_protocol, type); return -1;
        }
        if (topology->content_protocol != type) {
            topology->content_protocol = (uint8_t)type;
            DEBUG_INFO(DEBUG_CATEGORY_GROUP, "group protocol published ch=%s type=%s", group,
                type == GROUP_PROTOCOL_CHAT ? "chat" : "kanban");
        }
        struct topo_peer_ready_cbk* cb = topology->peer_ready_cbks;
        while (cb && (cb->fn != content_ready || cb->arg != inst)) cb = cb->next;
        if (!cb && topo_group_add_peer_ready_cbk(topology, content_ready, inst)) return -1;
    }
    if (type <= 0) return type;
    if (type == 2) { DEBUG_DEBUG(DEBUG_CATEGORY_GROUP, "kanban runtime activated ch=%s", group); return 0; }
    if (!inst->chat_content_started) return 0;
    struct DB_SYNC_INSTANCE* si = si_find(inst, group);
    if (!si) {
        if (strcmp(prepared->history.channel, group)) {
            DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "CHAT activation has no committed history ch=%s", group); return -1;
        }
        si = db_sync_instance_activate(inst, &prepared->history);
        if (!si) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "chat history preparation failed ch=%s", group); return -1; }
        db_sync_set_insert_cb(si, on_msg_inserted, inst);
    }
    /* После рестарта содержимого группа может уже иметь READY-сессии без нового перехода READY. */
    if (merkle_sync_registered(inst, MT_DATA_CHAT_LOG)) {
        for (struct ll_entry* e = topology && topology->senders_list ? topology->senders_list->head : NULL; e; e = e->next) {
            struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
            if (peer->conn) group_protocol_peer_ready(inst, group, peer->conn->peer_ed25519_pubkey);
        }
    }
    return 0;
}
int group_protocol_prepare(struct UTUN_INSTANCE* inst, const char* group) {
    if (!inst || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group legacy preparation requires synchronous SQL owner ch=%s", group ? group : "?"); return -1;
    }
    sqlite3* db = inst->topo_sqlite_db; int own = db && sqlite3_get_autocommit(db);
    struct group_protocol_prepared prepared;
    int rc = !db ? SQLITE_MISUSE : own ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_OK;
    if (!rc) rc = group_protocol_prepare_write(db, group, inst->chat_content_started, &prepared);
    if (!rc && own) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group legacy preparation ch=%s rc=%d", group, rc);
        if (own && !sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL)) utun_instance_stop(inst);
        return -1;
    }
    return group_protocol_activate(inst, group, &prepared);
}
int group_protocol_archive(sqlite3* db, uint64_t self, const char* group) {
    int type = group_protocol_type(db, group);
    (void)self;
    if (type == 1) { int changed=0; int rc = chat_log_rebuild_changed(db, group, &changed); return rc ? rc : changed; }
    return type < 0 ? -SQLITE_ERROR : 0;
}
void group_protocol_archived(struct UTUN_INSTANCE* inst, const char* group) {
    int type = group_protocol_current(inst, group);
    if (!type && inst && !db_async_can_write(inst->db_worker)) type = group_protocol_type(inst->topo_sqlite_db, group);
    if (type == 1) { chat_log_publish(inst, group); }
}
void group_protocol_members_changed(struct UTUN_INSTANCE* inst, const char* group) {
    int type = group_protocol_current(inst, group);
    if (!type && inst && !db_async_can_write(inst->db_worker)) type = group_protocol_type(inst->topo_sqlite_db, group);
    if (type == 2) kanban_members_changed(inst, group);
    else if (type == 1) {
        chat_log_changed(inst, group);
        merkle_sync_revalidate_peers(inst, MT_DATA_CHAT_LOG, group);
        db_sync_members_changed(inst, group);
    }
}
void group_protocol_peer_ready(struct UTUN_INSTANCE* inst, const char* group, const uint8_t peer[32]) {
    member_sync_audit_invitation(inst, group, NULL);
    int type = group_protocol_current(inst, group);
    if (type == 2) { kanban_peer_ready(inst, group, peer); return; }
    if (type != 1 || !inst->chat_content_started) return;
    struct DB_SYNC_INSTANCE* si = si_find(inst, group); if (si) db_sync_instance_set_gated(si, 0);
    chat_log_peer_ready(inst, group, peer);

}
int group_protocol_seal_role_write(sqlite3* db, const struct member_sync_write_context* context,
    const char* group, const uint8_t member[32], char** json) {
    if (!db || !context || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "owner seal requires SQL context/caller transaction"); return -1;
    }
    int type = group_protocol_type(db, group);
    if (type != 2) return type < 0 ? -1 : 0;
    uint8_t generation[32]; char head[65]; uint64_t epoch;
    if (!json || !*json || group_policy_key_generation(db, group, member, generation) ||
        member_sync_owner_epoch(*json, &epoch) || epoch == INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "owner seal invalid generation/epoch ch=%s", group); return -1;
    }
    struct kanban_identity writer = {.now=(int64_t)context->now};
    memcpy(writer.public_key, context->public_key, 32); memcpy(writer.private_key, context->private_key, 32);
    int sealed = kanban_seal_write(db, &writer, group, member, generation, head); OPENSSL_cleanse(&writer, sizeof(writer));
    if (sealed) return -1;
    sqlite3_stmt* st = NULL; char next[32]; snprintf(next, sizeof(next), "%llu", (unsigned long long)(epoch + 1));
    int rc = sqlite3_prepare_v2(db,
        "SELECT json_set(?1,'$._message_epoch',?2,'$._kanban_seal',?3)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, *json, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, next, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, head, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    const char* value = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
    char* updated = value && strlen(value) <= MS_ADM_TAGS_MAX ? u_strdup(value) : NULL;
    int final = sqlite3_finalize(st);
    if (!updated || final != SQLITE_OK) { u_free(updated); DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "owner seal update failed ch=%s", group); return -1; }
    u_free(*json); *json = updated; return 0;
}

int group_protocol_seal_role(struct UTUN_INSTANCE* inst, const char* group, const uint8_t member[32], char** json) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "owner seal missing instance"); return -1; }
    struct member_sync_write_context context = {.self=inst->node_id, .now=ntp_time_get_us(inst)};
    memcpy(context.public_key, inst->my_ed25519_pubkey, 32); memcpy(context.private_key, inst->my_ed25519_privkey, 32);
    int rc = group_protocol_seal_role_write(inst->topo_sqlite_db, &context, group, member, json);
    OPENSSL_cleanse(&context, sizeof(context)); return rc;
}
