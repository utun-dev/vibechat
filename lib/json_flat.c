#include "json_flat.h"
#include "mem.h"
#include "debug_config.h"
#include <string.h>

#define SKIP_WS(p, end) while ((p) < (end) && (*(p) == ' ' || *(p) == '\t' || *(p) == '\n' || *(p) == '\r')) (p)++
#define CHECK(p, end)   if ((p) >= (end)) return -1

static int json_unicode_unit(const char** text, const char* end, uint32_t* value) {
    if (end - *text < 4) return -1;
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)(*text)[i]; int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return -1;
        result = (result << 4) | (uint32_t)digit;
    }
    *text += 4; *value = result; return 0;
}

static int json_read_string(const char** pp, const char* end, char* out, size_t out_sz) {
    const char* p = *pp;
    CHECK(p, end);
    if (*p != '"') return -1; p++;
    size_t n = 0;
    while (p < end && *p != '"') {
        if ((unsigned char)*p < 32) return -1;
        char ch = *p;
        if (*p == '\\') { p++; CHECK(p, end);
            if (*p == 'u') {
                uint32_t code; p++; if (json_unicode_unit(&p, end, &code) < 0 || !code) return -1;
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return -1;
                    p += 2; uint32_t low;
                    if (json_unicode_unit(&p, end, &low) < 0 || low < 0xdc00 || low > 0xdfff) return -1;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return -1;
                uint8_t utf8[4]; size_t bytes;
                if (code < 0x80) { utf8[0] = (uint8_t)code; bytes = 1; }
                else if (code < 0x800) {
                    utf8[0] = (uint8_t)(0xc0 | (code >> 6)); utf8[1] = (uint8_t)(0x80 | (code & 63)); bytes = 2;
                } else if (code < 0x10000) {
                    utf8[0] = (uint8_t)(0xe0 | (code >> 12)); utf8[1] = (uint8_t)(0x80 | ((code >> 6) & 63));
                    utf8[2] = (uint8_t)(0x80 | (code & 63)); bytes = 3;
                } else {
                    utf8[0] = (uint8_t)(0xf0 | (code >> 18)); utf8[1] = (uint8_t)(0x80 | ((code >> 12) & 63));
                    utf8[2] = (uint8_t)(0x80 | ((code >> 6) & 63)); utf8[3] = (uint8_t)(0x80 | (code & 63)); bytes = 4;
                }
                if (out) {
                    if (n + bytes >= out_sz) return -1;
                    memcpy(out + n, utf8, bytes); n += bytes;
                }
                continue;
            }
            switch (*p) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                default: return -1;
            }
        }
        if (out && n < out_sz - 1) out[n++] = ch;
        p++;
    }
    CHECK(p, end); /* closing quote */
    p++;
    if (out && n < out_sz) out[n] = '\0';
    else if (out && out_sz > 0) out[out_sz - 1] = '\0';
    *pp = p;
    return 0;
}

static int json_parse_pair(const char** pp, const char* end, char* scratch, size_t capacity,
                            json_flat_cb cb, void* arg) {
    const char* p = *pp;
    SKIP_WS(p, end); CHECK(p, end);
    if (*p == '}') { *pp = p; return 0; }

    char* key = scratch;
    if (json_read_string(&p, end, key, capacity) != 0) return -1;
    size_t key_size = strlen(key) + 1;
    char* val = scratch + key_size;
    SKIP_WS(p, end); CHECK(p, end);
    if (*p != ':') return -1; p++;
    SKIP_WS(p, end); CHECK(p, end);
    if (json_read_string(&p, end, val, capacity - key_size) != 0) return -1;

    if (cb) { int rc = cb(key, val, arg); if (rc != 0) { *pp = p; return rc < 0 ? -1 : 1; } }

    SKIP_WS(p, end); CHECK(p, end);
    if (*p == ',') p++;

    *pp = p;
    return 0;
}

int json_flat_parse(const char* json, json_flat_cb cb, void* arg) {
    if (!json) return -1;
    const char* p = json, *end = json + strlen(json);
    SKIP_WS(p, end); CHECK(p, end);
    if (*p != '{') return -1; p++;

    /* Decoded key + value + two NULs fit within the encoded object size. */
    size_t capacity = strlen(json) + 1;
    if (capacity > UINT32_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "json_flat: input too large"); return -1; }
    char local[384];
    char* scratch = capacity <= sizeof(local) ? local : u_malloc(capacity);
    if (!scratch) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "json_flat: allocation failed bytes=%zu", capacity); return -1; }
    int result = 0;
    for (;;) {
        int rc = json_parse_pair(&p, end, scratch, capacity, cb, arg);
        if (rc < 0) { result = -1; break; }
        if (rc > 0) break;
        SKIP_WS(p, end);
        if (p >= end) { result = -1; break; }
        if (*p == '}') break;
    }
    if (scratch != local) u_free(scratch);
    if (result < 0) DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "json_flat: invalid object/string bytes=%zu", capacity - 1);
    return result;
}

struct get_ctx { const char* key; char* out; size_t sz; int found; };

static int get_cb(const char* k, const char* v, void* arg) {
    struct get_ctx* g = (struct get_ctx*)arg;
    if (strcmp(k, g->key) == 0) { if (g->out) { size_t vl = strlen(v);
        size_t cp = vl < g->sz - 1 ? vl : g->sz - 1; memcpy(g->out, v, cp); g->out[cp] = '\0'; }
        g->found = 1; return 1; }
    return 0;
}

int json_flat_get(const char* json, const char* key, char* value_out, size_t value_sz) {
    if (!key || (value_out && !value_sz)) return -1;
    struct get_ctx g = { key, value_out, value_sz, 0 };
    if (json_flat_parse(json, get_cb, &g) != 0) return -1;
    return g.found ? 0 : -1;
}
