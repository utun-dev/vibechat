/* Обычный picker-файл: общий worker владеет cache и подтверждает публикацию. */
#include <stdio.h>
#include <string.h>
#include "attachment_sender.h"
#include "instance_lite.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../lib/u_async.h"
#include "../../../src/media_async/attachment_send.h"

int attachment_send(const char* channel_id, const char* src_file_path, const char* db_path,
                    const uint8_t reply_to[32], uint64_t request_id, const char* reply_quote) {
    (void)db_path;
    if (!channel_id || !src_file_path || !reply_to || strlen(channel_id) > 64 || strlen(src_file_path) >= 1024 ||
        (reply_quote && strlen(reply_quote) > DB_SYNC_MESSAGE_MAX) ||
        !instance_lite_is_running()) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment_send: invalid arguments/stopped core"); return -1;
    }
    const char* name = strrchr(src_file_path, '/'); name = name ? name + 1 : src_file_path;
    struct attachment_send_req* req = u_calloc(1, sizeof(*req));
    if (!req) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment_send: allocation failed"); return -1;
    }
    if (strlen(name) >= sizeof(req->info.name)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment_send: filename too long"); u_free(req); return -1;
    }
    req->inst = instance_lite_get_instance(); req->info.kind = ATTACHMENT_FILE;
    snprintf(req->info.name, sizeof(req->info.name), "%s", name);
    if (attachment_validate(&req->info)) { u_free(req); return -1; }
    snprintf(req->target, sizeof(req->target), "%s", channel_id);
    snprintf(req->source, sizeof(req->source), "%s", src_file_path);
    memcpy(req->reply_to, reply_to, sizeof(req->reply_to)); req->request_id = request_id;
    if (reply_quote) strcpy(req->reply_quote, reply_quote);
    req->temporary_source = 1;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment_send: accepted channel=%s source=%s request=%llu",
               channel_id, src_file_path, (unsigned long long)request_id);
    if (instance_lite_post(attachment_send_trampoline, req, &req->inst, attachment_send_discard)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment_send: worker rejected request=%llu", (unsigned long long)request_id);
        u_free(req); return -1;
    }
    return 0;
}
