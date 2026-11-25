/*
 * chat_event.c — реализация системы нотификаций
 *
 * Обработчик — per-instance (хранится в UTUN_INSTANCE.chat_event_handler).
 * Пока обработчик не зарегистрирован — события логируются.
 * При регистрации обработчика — форвардятся ему синхронно.
 */

#include "chat_event.h"
#include "../utun_instance.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

void chat_event_set_handler(struct UTUN_INSTANCE* inst, chat_event_handler_fn handler) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "chat_event: handler requires instance"); return; }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "utun-evt", "chat_event_set_handler: handler=%p old=%p",
                        (void*)handler, (void*)inst->chat_event_handler);
#endif
    inst->chat_event_handler = handler;
}

void chat_event_message(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t author[32], int initial) {
    uint8_t event[1 + 64 + CHAT_MSG_EVENT_SUFFIX_SIZE] = {0};
    size_t n = channel ? strlen(channel) : 0;
    if (n != 64) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "chat_event: invalid message channel length=%zu", n); return;
    }
    event[0] = (uint8_t)n;
    memcpy(event + 1, channel, n);
    if (author) memcpy(event + 1 + n, author, 32);
    event[n + CHAT_MSG_EVENT_SUFFIX_SIZE] = initial != 0;
    chat_event_post(inst, CHAT_EVT_MSG_RECEIVED, event, (int)n + 1 + CHAT_MSG_EVENT_SUFFIX_SIZE);
}

void chat_event_post(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (!inst || type <= 0 || len < 0 || (len && !data)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "chat_event: invalid event instance=%p type=%d bytes=%d data=%p",
            (void*)inst, type, len, (const void*)data); return;
    }
    if (inst->chat_event_handler) { inst->chat_event_handler(inst, type, data, len); return; }

    static const char* names[] = {
        [1]="MSG_RECEIVED",  [2]="CONNECT_RESULT",
        [4]="CHANNEL_UPDATED", [5]="MEMBERS_CHANGED",
        [7]="AUTO_CONNECT_STATUS", [8]="CHANNEL_PEERS_ONLINE",
        [9]="DB_READY",     [10]="STATUS_REFRESH", [11]="KEYS_GENERATED",
        [12]="SERVICE_STARTED", [13]="SERVICE_STOPPED",
        [14]="ATTACHMENT_DOWNLOADED", [15]="DOWNLOAD_PROGRESS",
        [16]="NODEINFO_UPDATED", [17]="LINK_STATUS_CHANGED",
        [18]="INVITE_RECEIVED", [19]="LOCAL_SOCKETS",
        [20]="CONN_LIST", [21]="CONN_METRICS",
        [22]="MEMBER_DETAIL", [23]="MEMBER_LIST",
        [24]="MEMBER_UPDATED", [25]="MEMBER_REMOVED",
        [27]="CONNECTING_NODES",
        [28]="ADMIN_KEY_RECEIVED", [29]="INVITE_LINK_READY",
        [30]="INVITE_CANDIDATES", [31]="CHANNEL_DELETED",
        [32]="DM_MSG_RECEIVED", [33]="DM_CONV_UPDATED",
        [34]="CALL_INCOMING", [35]="CALL_RINGING", [36]="CALL_ACCEPTED",
        [37]="CALL_ENDED", [38]="CALL_DECLINED", [39]="CALL_STATS",
        [40]="CALL_ERROR", [41]="DM_MESSAGES", [42]="CALL_PATH",
        [43]="RADIO_TALK",
        [44]="CALL_CONNECTION",
        [45]="DM_CONVERSATIONS", [46]="ATTACHMENT_PREPARATION", [47]="DM_CALL_ROUTE",
        [48]="DM_INVITE_RESULT", [49]="MEMBER_FILES_RESPONSE", [50]="MEMBER_FILES_TRANSFER",
        [51]="AVATAR_RESULT", [52]="GROUP_AVATAR_RESULT", [53]="FEEDBACK_CHANGED", [54]="FEEDBACK_RESULT",
        [55]="RADIO_MUTE_RESULT",
        [56]="MSG_SUBMITTED",
        [57]="CPU_LOAD",
        [58]="STREAM_SOURCES",
        [59]="DM_DELETE_RESULT",
        [60]="ATTACHMENT_KEYS_CHANGED",
        [61]="LOCAL_DEVICES", [62]="LOCAL_INVITE", [63]="LOCAL_DELIVERY", [64]="LOCAL_DISCOVERY_ACTIVE",
        [65]="RADIO_CALL", [66]="CALL_PHONE_BUSY", [67]="MSG_UPDATED",
        [69]="NETWORK_STATS", [70]="DEVGROUP_CHANGED", [71]="DEVGROUP_RESULT",
        [77]="CLIPBOARD_RECEIVED", [78]="CLIPBOARD_RESULT",
        [79]="KANBAN_CHANGED", [80]="KANBAN_RESULT",
    };
    const char* n = (type >= 1 && type < (int)(sizeof(names) / sizeof(names[0]))) ? names[type] : NULL;
    if (!n) n = "?";
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "chat_event: %s(%d) data=%d bytes", n, type, len);
    if (data && len > 0 && type != CHAT_EVT_CLIPBOARD_RECEIVED && type != CHAT_EVT_DEVGROUP_RESULT &&
        type != CHAT_EVT_LOCAL_INVITE && type != CHAT_EVT_INVITE_LINK_READY && type != CHAT_EVT_KANBAN_RESULT)
        log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CHAT, "  ", data, len);
}

/* Идентификатор связывает результат с конкретной отправкой, независимо от текущего канала GUI. */
void chat_event_submitted(struct UTUN_INSTANCE* inst, uint64_t request_id, int result) {
    if (!request_id) return;
    uint8_t data[9];
    for (unsigned i = 0; i < 8; i++) data[i] = (uint8_t)(request_id >> (8 * i));
    if (result < 0 || result > 2) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "invalid submission result=%d", result); result = 0; }
    data[8] = (uint8_t)result;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "message submit result: request=%llu result=%d", (unsigned long long)request_id, result);
    chat_event_post(inst, CHAT_EVT_MSG_SUBMITTED, data, sizeof(data));
}

/* ID задачи не даёт позднему завершению изменить статус другой подготовки. */
void chat_event_attachment(struct UTUN_INSTANCE* inst, int is_dm, const char* target, const uint8_t id[16], int state) {
    if (!target || !id) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: preparation event requires target and UUID"); return; }
    char name[68];
    int n = snprintf(name, sizeof(name), "%s%s", is_dm ? "dm:" : "", target);
    if (n <= 0 || (size_t)n >= sizeof(name) || state < 0 || state > 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: invalid preparation event target=%s state=%d", target, state); return;
    }
    uint8_t data[1 + 68 + 16 + 1];
    data[0] = (uint8_t)n; memcpy(data + 1, name, (size_t)n); memcpy(data + 1 + n, id, 16); data[17 + n] = (uint8_t)state;
    chat_event_post(inst, CHAT_EVT_ATTACHMENT_PREPARATION, data, n + 18);
}
