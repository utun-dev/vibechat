#include "group_protocol.h"
#include "../chat/chat_core_priv.h"
#include "../chat/chat_sync.h"
#include "../chat/chat_join.h"
#include "../chat/member_sync.h"
#include "../dm/dm_core.h"
#include "../kanban/kanban.h"
#include "../attachments/service.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_group.h"
#include "../../lib/ll_queue.h"
#include "../../lib/db_async.h"

/* Догнать аудит текущего приглашения после перезапуска, в том числе без READY-пиров. */
static void group_started(struct UTUN_INSTANCE* inst) {
    inst->group_started = 1;
    for (struct ll_entry* e = inst->topo_groups ? inst->topo_groups->group_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (group->group_type == TOPO_GROUP_TYPE_CHAT && group->content_protocol)
            member_sync_audit_invitation(inst, group->channel_id, NULL);
    }
}

int group_service_start(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->core_started) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group service requires running core and committed SQLite"); return -1;
    }
    if (inst->group_started) return 0;
    if (db_async_can_write(inst->db_worker)) {
        if (!chat_core_is_initialized(inst) || !chat_sync_is_initialized(inst) || !inst->msync || !inst->attachments) {
            DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group service requires activated core/MEMBERS/chat sync after SQL bootstrap"); return -1;
        }
        group_started(inst);
        DEBUG_INFO(DEBUG_CATEGORY_GROUP, "group service activated from committed worker snapshots"); return 0;
    }
    if (!inst->topo_sqlite_db || !sqlite3_get_autocommit(inst->topo_sqlite_db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group service legacy start requires committed SQLite"); return -1;
    }
    char path[512]; snprintf(path, sizeof(path), "%s/chats.db", inst->config->global.db_path);
    if (chat_core_init(inst, path) || chat_sync_init(inst) || attachment_service_start(inst, NULL)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group service start failed; releasing partial runtime"); group_service_stop(inst); return -1;
    }
    group_started(inst);
    DEBUG_INFO(DEBUG_CATEGORY_GROUP, "group service started: JOIN, MEMBERS, roles and group metadata"); return 0;
}

void group_service_stop(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    inst->group_started = 0; inst->chat_started = 0; inst->chat_content_started = 0;
    dm_core_quiesce(inst);
    chat_core_quiesce(inst);
    chat_sync_quiesce(inst); chat_join_destroy(inst); kanban_service_stop(inst); chat_service_stop(inst); chat_sync_destroy(inst);
    attachment_service_stop(inst);
    struct ll_entry* entry = inst->topo_groups ? inst->topo_groups->group_list->head : NULL;
    while (entry) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)entry; entry = entry->next;
        if (group->group_type == TOPO_GROUP_TYPE_CHAT) topo_groups_remove_group(inst->topo_groups, group->group_id);
    }
    chat_core_destroy(inst); inst->group_started = 0;
    DEBUG_INFO(DEBUG_CATEGORY_GROUP, "group service stopped; SQLite retained by core");
}
