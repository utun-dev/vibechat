#include "photo_sender.h"
#include "instance_lite.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../lib/u_async.h"
#include "../../../src/chat/chat_core.h"
#include "../../../src/media_async/attachment_send.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

int photo_send(const char* channel_id, const char* src_file_path, const char* db_path,
               int width, int height, const uint8_t reply_to[32], uint64_t request_id,
               const char* content_type, uint64_t generation, const char* reply_quote) {
    if (!channel_id || !src_file_path || !db_path || !content_type || !reply_to ||
        (reply_quote && strlen(reply_quote) > DB_SYNC_MESSAGE_MAX) ||
        width <= 0 || height <= 0 || width > 16384 || height > 16384 || strlen(src_file_path) >= 1024) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "photo_send: invalid args");
        return -1;
    }

    if (!instance_lite_is_running()) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "photo_send: core stopped"); return -1; }

    struct stat st; memset(&st, 0, sizeof(st));
    if (stat(src_file_path, &st) != 0 || st.st_size <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "photo_send: source not found or empty: %s", src_file_path);
        return -1;
    }

    if (!strcmp(content_type, "image/jpeg") || !strcmp(content_type, "image/png")) {
        struct attachment_send_req* req = u_calloc(1, sizeof(*req));
        if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "photo_send: allocation failed"); return -1; }
        req->inst = instance_lite_get_instance(); req->is_dm = !strncmp(channel_id, "dm:", 3);
        const char* target = channel_id + (req->is_dm ? 3 : 0);
        const char* name = strrchr(src_file_path, '/'); name = name ? name + 1 : src_file_path;
        if (strlen(target) >= sizeof(req->target) || strlen(name) >= sizeof(req->info.name) || (req->is_dm && !generation)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "photo_send: invalid target/name/generation"); u_free(req); return -1;
        }
        req->dm_generation = generation; req->temporary_source = 1;
        req->info.kind = !strcmp(content_type, "image/png") ? ATTACHMENT_PNG : ATTACHMENT_JPEG;
        req->info.width = (uint16_t)width; req->info.height = (uint16_t)height;
        snprintf(req->target, sizeof(req->target), "%s", target);
        snprintf(req->source, sizeof(req->source), "%s", src_file_path);
        snprintf(req->info.name, sizeof(req->info.name), "%s", name);
        if (attachment_validate(&req->info)) { u_free(req); return -1; }
        req->request_id = request_id;
        if (!req->is_dm) memcpy(req->reply_to, reply_to, 32);
        if (!req->is_dm && reply_quote) strcpy(req->reply_quote, reply_quote);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "photo_send: target=%s mime=%s dimensions=%dx%d bytes=%lld generation=%llu",
                   channel_id, content_type, width, height, (long long)st.st_size, (unsigned long long)generation);
        if (instance_lite_post(attachment_send_trampoline, req, &req->inst, attachment_send_discard)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "photo_send: worker rejected request=%llu", (unsigned long long)request_id);
            u_free(req); return -1;
        }
        return 0;
    }
    if (strncmp(channel_id, "dm:", 3) == 0 ||
        (strcmp(content_type, "image/gif") && strcmp(content_type, "image/bmp") && strcmp(content_type, "image/webp"))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "photo_send: unsupported image target=%s mime=%s", channel_id, content_type); return -1;
    }

    char path_copy[1024];
    snprintf(path_copy, sizeof(path_copy), "%s", src_file_path);
    char* fname = basename(path_copy);
    char original_basename[256];
    snprintf(original_basename, sizeof(original_basename), "%s", fname);
    char* dot = strrchr(fname, '.');
    if (dot) *dot = '\0';

    const char* use_ext = "jpg";
    if (!strcmp(content_type, "image/gif")) use_ext = "gif";
    else if (!strcmp(content_type, "image/bmp")) use_ext = "bmp";
    else if (!strcmp(content_type, "image/webp")) use_ext = "webp";

    time_t t = time(NULL);
    struct tm tm_buf; localtime_r(&t, &tm_buf);
    char dt[32]; snprintf(dt, sizeof(dt), "%04d%02d%02d-%02d%02d%02d",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    int rnd = rand() & 0xFFFF;
    char suffix[8]; snprintf(suffix, sizeof(suffix), "%04x", rnd);

    /* ensure media dir exists */
    char media_dir[1024];
    snprintf(media_dir, sizeof(media_dir), "%s/media/%s", db_path, channel_id);
    {
        char tmp[1024]; size_t off = 0;
        for (size_t i = 0; media_dir[i] && off < sizeof(tmp) - 1; i++) {
            tmp[off++] = media_dir[i];
            if (media_dir[i] == '/' && off > 1) { tmp[off] = '\0'; mkdir(tmp, 0755); }
        }
        tmp[off] = '\0';
        mkdir(tmp, 0755);
    }

    char media_file[1024];
    snprintf(media_file, sizeof(media_file), "%s/%s_%s_%s.%s", media_dir, dt, fname, suffix, use_ext);

    size_t datalen = strlen(original_basename);
    struct chat_msg_submit* req = u_calloc(1, sizeof(struct chat_msg_submit) + datalen + 1);
    if (!req) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "photo_send: allocation failed"); return -1;
    }

    req->inst = instance_lite_get_instance();
    memcpy(req->reply_to, reply_to, sizeof(req->reply_to));
    if (reply_quote) strcpy(req->reply_quote, reply_quote);
    req->request_id = request_id;
    snprintf(req->channel_id, sizeof(req->channel_id), "%s", channel_id);
    snprintf(req->content_type, sizeof(req->content_type), "%s", content_type);
    snprintf(req->media_src, sizeof(req->media_src), "%s", src_file_path);
    snprintf(req->media_dest, sizeof(req->media_dest), "%s", media_file);
    req->media_copy = 1;
    req->media_video = 0;
    req->width = width;
    req->height = height;
    req->data = (uint8_t*)(req + 1);
    memcpy(req->data, original_basename, datalen);
    req->data_len = (uint32_t)datalen;
    req->timestamp = 0;

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "photo_send: ch=%s ct=%s src=%s dst=%s name=%s size=%lld %dx%d",
               channel_id, content_type, src_file_path, media_file, original_basename,
               (long long)st.st_size, width, height);

    if (instance_lite_post(chat_core_submit_trampoline, req, &req->inst, NULL)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "photo_send: worker rejected request=%llu", (unsigned long long)request_id);
        u_free(req); return -1;
    }
    return 0;
}
