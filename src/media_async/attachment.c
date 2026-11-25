/* Ограниченный бинарный формат: kind:1, name_len:2, duration_ms:4, width:2,
 * height:2, UTF-8 name, waveform:100 для voice. Числа big-endian. */
#include <string.h>
#include "../../lib/debug_config.h"
#include "attachment.h"

/* Корректный UTF-8 basename без ASCII C0/DEL и разделителей пути. */
static int valid_name(const char* name, size_t n) {
    if (!n || n > 255 || (n == 1 && name[0] == '.') || (n == 2 && !memcmp(name, "..", 2))) return 0;
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)name[i++];
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return 0;
        if (c < 0x80) continue;
        unsigned count, scalar, minimum;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; scalar = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; scalar = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; scalar = c & 7; minimum = 0x10000; }
        else return 0;
        if (count > n - i) return 0;
        while (count--) {
            c = (unsigned char)name[i++];
            if ((c & 0xc0) != 0x80) return 0;
            scalar = (scalar << 6) | (c & 63);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return 0;
    }
    return 1;
}

/* Один набор правил для отправки, приёма и снимка истории. */
int attachment_validate(const struct attachment_info* info) {
    if (!info || !valid_name(info->name, strnlen(info->name, sizeof(info->name)))) goto invalid;
    size_t wave = strnlen(info->waveform, sizeof(info->waveform));
    if (info->kind == ATTACHMENT_VOICE) {
        if (!info->duration_ms || info->duration_ms > ATTACHMENT_DURATION_MAX_MS || info->width || info->height || wave != 100) goto invalid;
        if (strspn(info->waveform, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") != wave) goto invalid;
    } else {
        if (wave) goto invalid;
        if (info->kind == ATTACHMENT_FILE) {
            if (info->duration_ms || info->width || info->height) goto invalid;
        } else if (info->kind == ATTACHMENT_VIDEO || info->kind == ATTACHMENT_JPEG || info->kind == ATTACHMENT_PNG) {
            if (!info->width || !info->height || info->width > 16384 || info->height > 16384) goto invalid;
            if (info->kind == ATTACHMENT_VIDEO) {
                if (!info->duration_ms || info->duration_ms > ATTACHMENT_DURATION_MAX_MS) goto invalid;
            } else if (info->duration_ms) goto invalid;
        } else goto invalid;
    }
    return 0;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: invalid metadata kind=%d dimensions=%ux%u duration=%u",
               info ? (int)info->kind : 0, info ? info->width : 0, info ? info->height : 0, info ? info->duration_ms : 0);
    return -1;
}

int attachment_encode(const struct attachment_info* info, uint8_t* out, size_t capacity) {
    if (attachment_validate(info)) return -1;
    size_t n = strlen(info->name), length = 11 + n + (info->kind == ATTACHMENT_VOICE ? 100 : 0);
    if (!out || capacity < length) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: encode buffer too small"); return -1; }
    out[0] = (uint8_t)info->kind; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)n;
    for (unsigned i = 0; i < 4; i++) out[3 + i] = (uint8_t)(info->duration_ms >> (24 - 8 * i));
    out[7] = (uint8_t)(info->width >> 8); out[8] = (uint8_t)info->width;
    out[9] = (uint8_t)(info->height >> 8); out[10] = (uint8_t)info->height;
    memcpy(out + 11, info->name, n);
    if (info->kind == ATTACHMENT_VOICE) memcpy(out + 11 + n, info->waveform, 100);
    return (int)length;
}

int attachment_decode(const uint8_t* data, size_t length, struct attachment_info* info) {
    if (!data || !info || length < 11) goto invalid;
    size_t n = ((size_t)data[1] << 8) | data[2];
    if (!n || n > 255 || length != 11 + n + (data[0] == ATTACHMENT_VOICE ? 100 : 0) || memchr(data + 11, 0, n)) goto invalid;
    memset(info, 0, sizeof(*info)); info->kind = (enum attachment_kind)data[0];
    for (unsigned i = 0; i < 4; i++) info->duration_ms = (info->duration_ms << 8) | data[3 + i];
    info->width = (uint16_t)((data[7] << 8) | data[8]); info->height = (uint16_t)((data[9] << 8) | data[10]);
    memcpy(info->name, data + 11, n);
    if (info->kind == ATTACHMENT_VOICE) memcpy(info->waveform, data + 11 + n, 100);
    return attachment_validate(info);
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: malformed wire metadata bytes=%zu", length);
    return -1;
}

const char* attachment_content_type(enum attachment_kind kind) {
    switch (kind) {
    case ATTACHMENT_VOICE: return "audio/opus";
    case ATTACHMENT_VIDEO: return "video/mp4";
    case ATTACHMENT_JPEG: return "image/jpeg";
    case ATTACHMENT_PNG: return "image/png";
    default: return "application/octet-stream";
    }
}

const char* attachment_info_content_type(const struct attachment_info* info) {
    size_t length = strlen(info->name);
    if (info->kind == ATTACHMENT_VIDEO && length >= 4 && !strcmp(info->name + length - 4, ".mkv")) return "video/x-matroska";
    return attachment_content_type(info->kind);
}
