/*
 * strbuf.c — safe growable printf buffer
 */
#include "strbuf.h"
#include "mem.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRBUF_MIN_CAP  16

void strbuf_init(struct strbuf *sb, size_t initial_cap) {
    *sb = strbuf_new();
    if (initial_cap > UINT32_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "strbuf: capacity exceeds allocator range=%zu", initial_cap); return; }
    if (initial_cap < STRBUF_MIN_CAP) initial_cap = STRBUF_MIN_CAP;
    sb->buf = u_malloc(initial_cap);
    if (sb->buf) { sb->buf[0] = '\0'; sb->cap = initial_cap; }
    else         { sb->cap = 0; }
    sb->len = 0;
}

void strbuf_free(struct strbuf *sb) {
    if (sb->buf) { u_free(sb->buf); sb->buf = NULL; }
    sb->cap = 0; sb->len = 0;
}

char *strbuf_detach(struct strbuf *sb) {
    char *ret = sb->buf;
    sb->buf = NULL; sb->cap = 0; sb->len = 0;
    return ret;
}

void strbuf_reset(struct strbuf *sb) {
    if (sb->buf) sb->buf[0] = '\0';
    sb->len = 0;
}

static int strbuf_grow(struct strbuf *sb, size_t need) {
    if (need > UINT32_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "strbuf: size exceeds allocator range=%zu", need); return -1; }
    size_t nc = sb->cap ? sb->cap : STRBUF_MIN_CAP;
    while (nc < need) { if (nc > UINT32_MAX / 2) { nc = need; break; } nc *= 2; }
    char *n = u_realloc(sb->buf, nc);
    if (!n) return -1;
    sb->buf = n; sb->cap = nc;
    return 0;
}

int strbuf_addf(struct strbuf *sb, const char *fmt, ...) {
    va_list ap;

    /* measure */
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) { return -1; }
    if ((size_t)need > UINT32_MAX - 1 - sb->len) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "strbuf: formatted text exceeds allocator range"); return -1;
    }

    /* grow */
    size_t want = sb->len + (size_t)need + 1;   /* +1 for '\0' */
    if (want > sb->cap && strbuf_grow(sb, want) < 0) return -1;

    /* write */
    va_start(ap, fmt);
    int w = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (w > 0) sb->len += (size_t)w;
    return w;
}

void strbuf_addc(struct strbuf *sb, char c) {
    if (sb->len > UINT32_MAX - 2) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "strbuf: character exceeds allocator range"); return; }
    if (sb->len + 2 > sb->cap && strbuf_grow(sb, sb->len + 2) < 0) return;
    sb->buf[sb->len++] = c;
    sb->buf[sb->len]   = '\0';
}

/* Экранирует ASCII-управляющие байты; UTF-8 сохраняется без преобразования. */
int strbuf_add_json_string(struct strbuf* sb, const char* text) {
    if (!sb || strbuf_addf(sb, "\"") < 0) goto failed;
    const unsigned char* p = (const unsigned char*)(text ? text : "");
    while (*p) {
        int rc;
        if (*p == '"' || *p == '\\') rc = strbuf_addf(sb, "\\%c", *p);
        else if (*p < 32) rc = strbuf_addf(sb, "\\u%04x", *p);
        else {
            const unsigned char* start = p;
            while (*p >= 32 && *p != '"' && *p != '\\') p++;
            rc = strbuf_addf(sb, "%.*s", (int)(p - start), start);
            if (rc < 0) goto failed;
            continue;
        }
        if (rc < 0) goto failed;
        p++;
    }
    if (strbuf_addf(sb, "\"") >= 0) return 0;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "strbuf: JSON string allocation failed"); return -1;
}

void strbuf_adds(struct strbuf *sb, const char *s) {
    if (!s) return;
    size_t slen = strlen(s);
    if (slen > UINT32_MAX - 1 - sb->len) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "strbuf: string exceeds allocator range"); return; }
    if (sb->len + slen + 1 > sb->cap && strbuf_grow(sb, sb->len + slen + 1) < 0) return;
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}
