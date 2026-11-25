#include "object_link.h"
#include "../../lib/debug_config.h"
#include <stdio.h>
#include <string.h>

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Один корректный Unicode scalar; суррогаты, overlong и оборванные последовательности запрещены. */
static size_t scalar(const unsigned char* text, size_t size, uint32_t* value) {
    if (!size) return 0;
    unsigned char c = text[0]; size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 :
        c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!n || n > size) return 0;
    uint32_t v = c & (n == 1 ? 0x7f : n == 2 ? 0x1f : n == 3 ? 0x0f : 7);
    for (size_t i = 1; i < n; i++) { if ((text[i] & 0xc0) != 0x80) return 0; v = (v << 6) | (text[i] & 63); }
    if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && v < 0x10000) ||
        v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff) || !v) return 0;
    *value = v; return n;
}

static int whitespace(uint32_t c) {
    return c <= 32 || (c >= 0x7f && c <= 0xa0) || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
        c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

static int unreserved(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~';
}

/* Разделители текста не могут находиться буквально внутри URL. */
static int literal(uint32_t c) {
    return !whitespace(c) && c != '<' && c != '>' && c != '"' && c != '\'' && c != '`' &&
        c != 0xab && c != 0xbb && c != 0x2018 && c != 0x2019 && c != 0x201c && c != 0x201d;
}

static int fail(char* error, size_t size, const char* reason) {
    if (error && size) snprintf(error, size, "%s", reason);
    return -1;
}

/* Валидация подписи после однократного %XX-декодирования; структурные разделители уже выделены. */
static int label_valid(const char* text, size_t size) {
    unsigned char decoded[OBJECT_LINK_MAX]; size_t count = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '%') {
            if (i + 2 >= size || hex_digit(text[i + 1]) < 0 || hex_digit(text[i + 2]) < 0) return 0;
            c = (unsigned char)((hex_digit(text[i + 1]) << 4) | hex_digit(text[i + 2])); i += 2;
        } else if (c < 0x80 && (!literal(c) || c == '?' || c == '#' || c == '\\')) return 0;
        decoded[count++] = c;
    }
    for (size_t i = 0; i < count;) {
        uint32_t code; size_t n = scalar(decoded + i, count - i, &code);
        if (!n) return 0;
        i += n;
    }
    /* Проверить также буквальные Unicode-разделители до декодирования. */
    for (size_t i = 0; i < size;) {
        if (text[i] == '%') { i += 3; continue; }
        uint32_t code; size_t n = scalar((const unsigned char*)text + i, size - i, &code);
        if (!n || !literal(code)) return 0;
        i += n;
    }
    return 1;
}

static int read_id(const char* text, size_t size, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    if (size != 64) return -1;
    for (size_t i = 0; i < size; i++) { int digit = hex_digit(text[i]); if (digit < 0) return -1; out[i] = hex[digit]; }
    out[64] = 0; return 0;
}

int object_link_parse(const char* url, size_t size, struct object_link_target* target, char* error, size_t error_size) {
    if (error && error_size) *error = 0;
    if (!url || !target || size > OBJECT_LINK_MAX || size < 12 || memchr(url, 0, size))
        return fail(error, error_size, "invalid URL length");
    struct object_link_target result = {0}; size_t start;
    if (size >= 12 && !memcmp(url, "utun://chat/", 12)) { result.kind = OBJECT_LINK_CHAT_MESSAGE; start = 12; }
    else if (size >= 14 && !memcmp(url, "utun://kanban/", 14)) { result.kind = OBJECT_LINK_CARD; start = 14; }
    else return fail(error, error_size, "unknown object link type");
    unsigned parts = 0;
    while (start < size) {
        size_t end = start, at = size;
        while (end < size && url[end] != '/') { if (url[end] == '@') at = end; end++; }
        if (at >= end || !label_valid(url + start, at - start)) return fail(error, error_size, "invalid label or separator");
        const char* id = url + at + 1; size_t length = end - at - 1;
        if (!parts) {
            if (read_id(id, length, result.group) || strspn(result.group, "0") == 64)
                return fail(error, error_size, "invalid group key");
        } else if (parts == 1) {
            if (read_id(id, length, result.kind == OBJECT_LINK_CHAT_MESSAGE ? result.message : result.card))
                return fail(error, error_size, "invalid object ID");
        } else if (parts == 2 && result.kind == OBJECT_LINK_CARD) {
            if (read_id(id, length, result.message)) return fail(error, error_size, "invalid message ID");
            result.kind = OBJECT_LINK_CARD_MESSAGE;
        } else return fail(error, error_size, "too many path segments");
        parts++; start = end + 1;
        if (end + 1 == size) return fail(error, error_size, "empty final segment");
    }
    if (parts < 2) return fail(error, error_size, "missing object ID");
    *target = result; return 0;
}

/* Короткая подпись без потери границы UTF-8; разделители и небезопасная пунктуация экранируются. */
static int append_label(char* out, size_t* used, const char* text) {
    if (!text) return 0;
    size_t size = strlen(text); unsigned count = 0; int separator = 0;
    for (size_t i = 0; i < size && count < 48;) {
        uint32_t code; size_t n = scalar((const unsigned char*)text + i, size - i, &code);
        if (!n) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "object link: label is not UTF-8 offset=%zu", i); return -1; }
        if (whitespace(code)) { if (count) separator = 1; i += n; continue; }
        if (separator && count < 47) { out[(*used)++] = '-'; count++; }
        separator = 0;
        int keep = unreserved(code) || (code >= 0x80 && literal(code));
        for (size_t j = 0; j < n; j++) {
            if (keep) out[(*used)++] = text[i + j];
            else { snprintf(out + *used, 4, "%%%02X", (unsigned char)text[i + j]); *used += 3; }
        }
        count++; i += n;
    }
    return 0;
}

int object_link_build(const struct object_link_target* target, const char* group_label, const char* card_label,
    const char* message_label, char* out, size_t out_size) {
    char group[65];
    if (!target || !out || !out_size || read_id(target->group, strnlen(target->group, 65), group) || strspn(group, "0") == 64 ||
        target->kind < OBJECT_LINK_CHAT_MESSAGE || target->kind > OBJECT_LINK_CARD_MESSAGE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "object link: invalid build target/output"); return -1;
    }
    char url[OBJECT_LINK_MAX + 1], normalized[65];
    size_t used = (size_t)snprintf(url, sizeof(url), "utun://%s/", target->kind == OBJECT_LINK_CHAT_MESSAGE ? "chat" : "kanban");
    if (append_label(url, &used, group_label)) return -1;
    used += (size_t)snprintf(url + used, sizeof(url) - used, "@%s/", group);
    unsigned parts = target->kind == OBJECT_LINK_CARD_MESSAGE ? 2 : 1;
    for (unsigned i = 0; i < parts; i++) {
        int card = target->kind != OBJECT_LINK_CHAT_MESSAGE && !i;
        const char* id = card ? target->card : target->message;
        if (read_id(id, strnlen(id, 65), normalized)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "object link: invalid build ID kind=%d part=%u", target->kind, i); return -1;
        }
        if (append_label(url, &used, card ? card_label : message_label)) return -1;
        used += (size_t)snprintf(url + used, sizeof(url) - used, "@%s%s", normalized, i + 1 < parts ? "/" : "");
    }
    if (used >= out_size) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "object link: output too small required=%zu capacity=%zu", used + 1, out_size); return -1; }
    memcpy(out, url, used + 1);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "object link built kind=%d group=%s bytes=%zu", target->kind, group, used);
    return (int)used;
}
