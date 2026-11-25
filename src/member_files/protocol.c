#include "protocol.h"
#include "../../lib/debug_config.h"
#include <string.h>
#include <ctype.h>

int mf_avatar_file_valid(const char* file) {
    if (!file || strlen(file) < 68 || strlen(file) > 69) return 0;
    const char* suffix = file + 64;
    if (strcmp(suffix, ".png") && strcmp(suffix, ".jpg") && strcmp(suffix, ".webp") && strcmp(suffix, ".json")) return 0;
    for (unsigned i = 0; i < 64; i++) if (!((file[i] >= '0' && file[i] <= '9') || (file[i] >= 'a' && file[i] <= 'f'))) return 0;
    return 1;
}

int mf_avatar_manifest_valid(const char* file) { return mf_avatar_file_valid(file) && !strcmp(file + 64, ".json"); }

/* Раздельные пределы метаданных и изображений; фактический хеш проверяет COMMIT. */
uint64_t mf_avatar_file_limit(const char* file) {
    if (!mf_avatar_file_valid(file)) return 0;
    return mf_avatar_manifest_valid(file) ? MF_AVATAR_MANIFEST_MAX_BYTES : MF_AVATAR_ORIGINAL_MAX_BYTES;
}

uint64_t mf_get64(const uint8_t* p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

void mf_put64(uint8_t* p, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (8 * i));
}

int mf_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/* Строгий UTF-8: без overlong, суррогатов и кодовых точек за Unicode. */
static int mf_utf8(const unsigned char* p) {
    while (*p) {
        unsigned c = *p++;
        if (c < 0x80) continue;
        unsigned count, value, minimum;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 7; minimum = 0x10000; }
        else return 0;
        for (unsigned i = 0; i < count; i++) {
            if ((*p & 0xc0) != 0x80) return 0;
            value = (value << 6) | (*p++ & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}

int mf_path_valid(const char* path, int root_allowed) {
    if (!path || strnlen(path, MF_PATH_MAX + 1) > MF_PATH_MAX) return 0;
    if (!*path) return root_allowed;
    if (!mf_utf8((const unsigned char*)path)) return 0;
    const char* start = path;
    unsigned components = 0;
    for (const char* p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && c != '/' && (c < 32 || c == 127 || strchr("\\:*?\"<>|", c))) return 0;
        if (c && c != '/') continue;
        size_t n = (size_t)(p - start);
        if (++components > 60) return 0;
        if (!n || n > MF_NAME_MAX || start[0] == '.' || start[n - 1] == '.' || start[n - 1] == ' ') return 0;
        char base[9]; size_t k = 0;
        while (k < n && k < sizeof(base) - 1 && start[k] != '.') { base[k] = (char)toupper((unsigned char)start[k]); k++; }
        base[k] = 0;
        if (!strcmp(base, "CON") || !strcmp(base, "PRN") || !strcmp(base, "AUX") || !strcmp(base, "NUL") ||
            (k == 4 && (!memcmp(base, "COM", 3) || !memcmp(base, "LPT", 3)) && base[3] >= '1' && base[3] <= '9')) return 0;
        if (!c) return 1;
        start = p + 1;
    }
}

int mf_encode(const struct mf_message* m, uint8_t* p, size_t cap) {
    size_t a = strnlen(m->path, sizeof(m->path)), b = strnlen(m->target, sizeof(m->target));
    size_t len = MF_WIRE_HEADER + a + b + m->data_len;
    if (a > MF_PATH_MAX || b > MF_PATH_MAX || m->data_len > MF_BLOCK_SIZE || cap < len) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: encode overflow paths=%zu/%zu data=%u cap=%zu", a, b, m->data_len, cap);
        return -1;
    }
    memset(p, 0, MF_WIRE_HEADER);
    p[0] = MF_PROTOCOL_VERSION; p[1] = m->kind; p[2] = m->command; p[3] = (uint8_t)m->status; p[4] = (uint8_t)(m->status >> 8);
    mf_put64(p + 5, m->request_id); memcpy(p + 13, m->owner, MF_ADDRESS_SIZE); mf_put64(p + 45, m->offset);
    mf_put64(p + 53, m->size); mf_put64(p + 61, m->cursor);
    memcpy(p + 69, m->version, 16); memcpy(p + 85, m->upload, 16); memcpy(p + 101, m->hash, 32);
    mf_put64(p + 133, m->mtime); p[141] = m->type; p[142] = m->more;
    p[143] = (uint8_t)a; p[144] = (uint8_t)(a >> 8); p[145] = (uint8_t)b; p[146] = (uint8_t)(b >> 8);
    p[147] = (uint8_t)m->data_len; p[148] = (uint8_t)(m->data_len >> 8);
    memcpy(p + MF_WIRE_HEADER, m->path, a); memcpy(p + MF_WIRE_HEADER + a, m->target, b);
    memcpy(p + MF_WIRE_HEADER + a + b, m->data, m->data_len);
    return (int)len;
}

int mf_decode(const uint8_t* p, size_t n, struct mf_message* m) {
    if (!p || !m || n < MF_WIRE_HEADER) goto bad;
    size_t a = p[143] | ((size_t)p[144] << 8), b = p[145] | ((size_t)p[146] << 8);
    size_t d = p[147] | ((size_t)p[148] << 8);
    if (p[0] != MF_PROTOCOL_VERSION || (p[1] != MF_REQUEST && p[1] != MF_RESPONSE) || p[2] < MF_LIST || p[2] > MF_QUOTA ||
        a > MF_PATH_MAX || b > MF_PATH_MAX || d > MF_BLOCK_SIZE || n != MF_WIRE_HEADER + a + b + d ||
        memchr(p + MF_WIRE_HEADER, 0, a + b) || p[142] > 1) goto bad;
    memset(m, 0, sizeof(*m));
    m->kind = p[1]; m->command = p[2]; m->status = p[3] | ((uint16_t)p[4] << 8);
    m->request_id = mf_get64(p + 5); memcpy(m->owner, p + 13, MF_ADDRESS_SIZE); m->offset = mf_get64(p + 45);
    m->size = mf_get64(p + 53); m->cursor = mf_get64(p + 61); m->mtime = mf_get64(p + 133);
    memcpy(m->version, p + 69, 16); memcpy(m->upload, p + 85, 16); memcpy(m->hash, p + 101, 32);
    m->type = p[141]; m->more = p[142]; m->data_len = (uint32_t)d;
    memcpy(m->path, p + MF_WIRE_HEADER, a); memcpy(m->target, p + MF_WIRE_HEADER + a, b);
    memcpy(m->data, p + MF_WIRE_HEADER + a + b, d);
    if (!m->request_id || m->status > MF_DIRECTORY_NOT_EMPTY || (m->kind == MF_REQUEST && m->status)) goto bad;
    return 0;
bad:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: malformed wire packet bytes=%zu", n);
    return -1;
}

const char* mf_status_text(int status) {
    static const char* texts[] = { "OK", "Access denied", "Not found", "Invalid path", "Invalid range", "Already exists",
        "Busy", "File changed", "Storage full", "I/O error", "Invalid packet", "Hash mismatch", "Cancelled", "Timeout",
        "Directory not empty" };
    return status >= 0 && status <= MF_DIRECTORY_NOT_EMPTY ? texts[status] : "Unknown error";
}

const char* mf_command_text(int command) {
    static const char* commands[] = { "SWEEP", "LIST", "STAT", "READ", "BEGIN_WRITE", "WRITE", "COMMIT", "ABORT",
                                      "MKDIR", "RENAME", "REMOVE", "STREAM_WRITE", "STREAM_READ", "DATA", "END", "CANCEL", "QUOTA" };
    return command >= 0 && command <= MF_QUOTA ? commands[command] : "UNKNOWN";
}
