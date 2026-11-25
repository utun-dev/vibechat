/* Android уже подготовил MP4 и прочитал метаданные в IO; ядро принимает фиксированный target. */
#include <stdio.h>
#include <string.h>
#include "../../../lib/mem.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/u_async.h"
#include "../../../src/media_async/attachment_send.h"
#include "video_sender.h"
#include "instance_lite.h"

int video_send(const char* channel_id, const char* src_file_path, const char* db_path,
               int duration_ms, int width, int height, const uint8_t reply_to[32], uint64_t request_id, uint64_t generation,
               const char* reply_quote) {
    (void)db_path;
    if (!channel_id || !src_file_path || !reply_to || !instance_lite_is_running() || duration_ms <= 0 || duration_ms > 86400000 ||
        (reply_quote && strlen(reply_quote) > DB_SYNC_MESSAGE_MAX) ||
        width <= 0 || width > 16384 || height <= 0 || height > 16384 || strlen(src_file_path) >= 1024) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "video send: invalid metadata/target or stopped core"); return -1;
    }
    struct attachment_send_req* req = u_calloc(1, sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "video send: allocation failed"); return -1; }
    req->dm_generation = generation;
    req->inst = instance_lite_get_instance(); req->is_dm = !strncmp(channel_id, "dm:", 3);
    req->request_id = request_id;
    if (!req->is_dm) memcpy(req->reply_to, reply_to, sizeof(req->reply_to));
    if (!req->is_dm && reply_quote) strcpy(req->reply_quote, reply_quote);
    snprintf(req->target, sizeof(req->target), "%s", channel_id + (req->is_dm ? 3 : 0));
    snprintf(req->source, sizeof(req->source), "%s", src_file_path);
    req->info.kind = ATTACHMENT_VIDEO; req->info.duration_ms = (uint32_t)duration_ms;
    req->info.width = (uint16_t)width; req->info.height = (uint16_t)height;
    req->temporary_source = 1; /* CameraX и picker создают принадлежащие приложению cache-файлы. */
    if (instance_lite_post(attachment_send_trampoline, req, &req->inst, attachment_send_discard)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "video send: worker rejected request=%llu", (unsigned long long)request_id);
        u_free(req); return -1;
    }
    return 0;
}
