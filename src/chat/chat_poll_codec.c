#include "chat_poll.h"
#include "chat_feedback.h"
#include "../../lib/debug_config.h"
#include <string.h>
#include <stdlib.h>

static const char* const reaction_catalog[CHAT_REACTION_COUNT] = {
    "👍", "👎", "❤️", "😂", "😮", "😢", "😡", "🎉", "🔥", "🤔", "💯", "👏", "🙏", "😍", "✨", "🤖"
};

const char* chat_reaction_emoji(unsigned id) { return id < CHAT_REACTION_COUNT ? reaction_catalog[id] : NULL; }

/* Проверяет UTF-8, исключая NUL, неверные/избыточные кодировки и surrogate. */
static int poll_utf8(const uint8_t* s, size_t len) {
    for (size_t i = 0; i < len;) {
        unsigned c = s[i++], value, bytes, minimum;
        if (c < 128) { if (!c || (c < 32 && c != 9 && c != 10 && c != 13)) return -1; continue; }
        if (c >= 0xc2 && c <= 0xdf) { value = c & 31; bytes = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { value = c & 15; bytes = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; bytes = 3; minimum = 0x10000; }
        else return -1;
        if (bytes > len - i) return -1;
        while (bytes--) { c = s[i++]; if ((c & 0xc0) != 0x80) return -1; value = (value << 6) | (c & 63); }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return -1;
    }
    return 0;
}

static int poll_error(const char* reason) {
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid data reason=%s", reason); return -1;
}

/* Проверка строки ограничена ёмкостью структуры; нет strlen за её границей. */
static int poll_string_size(const char* s, size_t capacity) {
    const char* end = memchr(s, 0, capacity);
    if (!end || poll_utf8((const uint8_t*)s, (size_t)(end - s)) < 0) return poll_error("text is not bounded UTF-8");
    return (int)(end - s);
}

/* Текст на проводе — BE16 длины и байты UTF-8. */
static int poll_put_string(const char* s, size_t capacity, uint8_t* out, size_t available) {
    int len = poll_string_size(s, capacity);
    if (len < 0) return -1;
    if (!len || available < (size_t)len + 2) return poll_error("empty text or poll exceeds size limit");
    out[0] = (unsigned)len >> 8; out[1] = len & 255; memcpy(out + 2, s, len);
    return len + 2;
}

static int poll_get_string(const uint8_t* data, size_t len, char* out, size_t capacity) {
    if (len < 2) return poll_error("truncated text length");
    size_t bytes = ((size_t)data[0] << 8) | data[1];
    if (!bytes || bytes >= capacity || bytes > len - 2 || poll_utf8(data + 2, bytes) < 0)
        return poll_error("invalid text length or UTF-8");
    memcpy(out, data + 2, bytes); out[bytes] = 0; return (int)bytes + 2;
}

/* Формат poll v1: version, mode, flags, option_count, question, options. */
int chat_poll_encode(const struct chat_poll* poll, uint8_t* out, size_t capacity) {
    if (!poll || !out || capacity < 6 || (poll->mode != CHAT_POLL_SINGLE && poll->mode != CHAT_POLL_MULTIPLE)
        || poll->allow_custom_answer > 1 || poll->option_count < 2 || poll->option_count > CHAT_POLL_OPTIONS_MAX)
        return poll_error("poll mode, flags or option count");
    if (capacity > CHAT_POLL_WIRE_MAX) capacity = CHAT_POLL_WIRE_MAX;
    out[0] = 1; out[1] = poll->mode; out[2] = poll->allow_custom_answer; out[3] = poll->option_count;
    int bytes = poll_put_string(poll->question, sizeof(poll->question), out + 4, capacity - 4);
    if (bytes < 0) return -1;
    size_t off = 4 + bytes;
    for (unsigned i = 0; i < poll->option_count; i++) {
        bytes = poll_put_string(poll->options[i], sizeof(poll->options[i]), out + off, capacity - off);
        if (bytes < 0) return -1;
        for (unsigned j = 0; j < i; j++) if (!strcmp(poll->options[i], poll->options[j])) return poll_error("duplicate option");
        off += bytes;
    }
    return (int)off;
}

int chat_poll_decode(const uint8_t* data, size_t len, struct chat_poll* poll) {
    if (!data || !poll || len < 6 || len > CHAT_POLL_WIRE_MAX || data[0] != 1
        || (data[1] != CHAT_POLL_SINGLE && data[1] != CHAT_POLL_MULTIPLE) || data[2] > 1
        || data[3] < 2 || data[3] > CHAT_POLL_OPTIONS_MAX) return poll_error("poll header");
    memset(poll, 0, sizeof(*poll)); poll->mode = data[1]; poll->allow_custom_answer = data[2]; poll->option_count = data[3];
    int bytes = poll_get_string(data + 4, len - 4, poll->question, sizeof(poll->question));
    if (bytes < 0) return -1;
    size_t off = 4 + bytes;
    for (unsigned i = 0; i < poll->option_count; i++) {
        bytes = poll_get_string(data + off, len - off, poll->options[i], sizeof(poll->options[i]));
        if (bytes < 0) return -1;
        for (unsigned j = 0; j < i; j++) if (!strcmp(poll->options[i], poll->options[j])) return poll_error("duplicate option");
        off += bytes;
    }
    return off == len ? 0 : poll_error("trailing poll bytes");
}

static int poll_compare_id(const void* a, const void* b) { return (int)*(const uint8_t*)a - *(const uint8_t*)b; }

/* Ответ: count, отсортированные номера, BE16 длины собственного текста, UTF-8. */
int chat_answer_encode(const struct chat_poll_answer* answer, uint8_t* out, size_t capacity) {
    if (!answer || !out || answer->count > CHAT_POLL_OPTIONS_MAX) return poll_error("answer count");
    int bytes = poll_string_size(answer->custom, sizeof(answer->custom));
    if (bytes < 0) return -1;
    if (!answer->count && !bytes) return 0;
    size_t len = 3 + answer->count + bytes;
    if (len > capacity) return poll_error("answer capacity");
    out[0] = answer->count; memcpy(out + 1, answer->choices, answer->count);
    qsort(out + 1, answer->count, 1, poll_compare_id);
    for (unsigned i = 1; i <= answer->count; i++)
        if (out[i] >= CHAT_POLL_OPTIONS_MAX || (i > 1 && out[i] == out[i - 1])) return poll_error("answer choices");
    out[1 + answer->count] = (unsigned)bytes >> 8; out[2 + answer->count] = bytes & 255;
    memcpy(out + 3 + answer->count, answer->custom, bytes); return (int)len;
}

int chat_answer_decode(const uint8_t* data, size_t len, struct chat_poll_answer* answer) {
    if (!answer || (len && !data)) return poll_error("answer arguments");
    memset(answer, 0, sizeof(*answer)); if (!len) return 0;
    if (len < 3 || data[0] > CHAT_POLL_OPTIONS_MAX || len < (size_t)data[0] + 3) return poll_error("answer header");
    size_t n = data[0], bytes = ((size_t)data[1 + n] << 8) | data[2 + n];
    if ((!n && !bytes) || bytes > CHAT_POLL_CUSTOM_MAX || len != 3 + n + bytes || poll_utf8(data + 3 + n, bytes) < 0)
        return poll_error("answer text or trailing bytes");
    for (size_t i = 1; i <= n; i++)
        if (data[i] >= CHAT_POLL_OPTIONS_MAX || (i > 1 && data[i] <= data[i - 1])) return poll_error("noncanonical choices");
    answer->count = n; memcpy(answer->choices, data + 1, n); memcpy(answer->custom, data + 3 + n, bytes); return 0;
}

int chat_reactions_encode(const uint8_t* ids, size_t count, uint8_t* out, size_t capacity) {
    if (!out || (count && !ids) || count > CHAT_REACTION_SELECTED_MAX) return poll_error("reaction count");
    if (!count) return 0;
    if (capacity < count + 1) return poll_error("reaction capacity");
    out[0] = count; memcpy(out + 1, ids, count); qsort(out + 1, count, 1, poll_compare_id);
    for (size_t i = 1; i <= count; i++)
        if (out[i] >= CHAT_REACTION_COUNT || (i > 1 && out[i] == out[i - 1])) return poll_error("reaction ids");
    return (int)count + 1;
}

int chat_reactions_decode(const uint8_t* data, size_t len, uint8_t ids[CHAT_REACTION_SELECTED_MAX], size_t* count) {
    if (!count || !ids || (len && !data)) return poll_error("reaction arguments");
    *count = 0; if (!len) return 0;
    if (!data[0] || data[0] > CHAT_REACTION_SELECTED_MAX || len != (size_t)data[0] + 1) return poll_error("reaction length");
    for (size_t i = 1; i < len; i++)
        if (data[i] >= CHAT_REACTION_COUNT || (i > 1 && data[i] <= data[i - 1])) return poll_error("noncanonical reactions");
    *count = data[0]; memcpy(ids, data + 1, *count); return 0;
}

int chat_feedback_validate_data(uint8_t kind, const uint8_t* data, size_t len) {
    if (kind == CHAT_FEEDBACK_REACTION) {
        uint8_t ids[CHAT_REACTION_SELECTED_MAX]; size_t count;
        return chat_reactions_decode(data, len, ids, &count);
    }
    if (kind == CHAT_FEEDBACK_ANSWER) {
        struct chat_poll_answer answer; return chat_answer_decode(data, len, &answer);
    }
    return poll_error("feedback kind");
}
