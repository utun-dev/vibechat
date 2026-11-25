#include "text_merge.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/strbuf.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct line { const char* data; size_t bytes; };
struct lines { struct line* items; size_t count; };
struct hunk { size_t first, last; const char* data; size_t bytes; };
struct diff { struct hunk* items; size_t count; };

static int split(const char* text, struct lines* lines) {
    size_t size = strlen(text);
    lines->items = u_calloc(size + 1, sizeof(*lines->items));
    if (!lines->items) return -1;
    for (size_t start = 0; start < size;) {
        const char* end = memchr(text + start, '\n', size - start);
        size_t bytes = end ? (size_t)(end - text) + 1 - start : size - start;
        lines->items[lines->count++] = (struct line){text + start, bytes}; start += bytes;
    }
    return 0;
}

static int same_line(const struct line* a, const struct line* b) {
    return a->bytes == b->bytes && !memcmp(a->data, b->data, a->bytes);
}

/* LCS с фиксированным выбором при равенстве: результат не зависит от платформы. */
static int difference(const struct lines* base, const struct lines* branch, struct diff* diff) {
    size_t width = branch->count + 1, cells = (base->count + 1) * width;
    uint16_t* lcs = u_calloc(cells, sizeof(*lcs));
    diff->items = u_calloc(base->count + branch->count + 1, sizeof(*diff->items));
    if (!lcs || !diff->items) { u_free(lcs); return -1; }
    for (size_t i = base->count; i-- > 0;) for (size_t j = branch->count; j-- > 0;) {
        uint16_t down = lcs[(i + 1) * width + j], right = lcs[i * width + j + 1];
        lcs[i * width + j] = same_line(&base->items[i], &branch->items[j])
            ? 1 + lcs[(i + 1) * width + j + 1] : down > right ? down : right;
    }
    size_t i = 0, j = 0;
    while (i < base->count || j < branch->count) {
        if (i < base->count && j < branch->count && same_line(&base->items[i], &branch->items[j])) { i++; j++; continue; }
        size_t first = i, start = j;
        while (i < base->count || j < branch->count) {
            if (i < base->count && j < branch->count && same_line(&base->items[i], &branch->items[j])) break;
            if (j < branch->count && (i == base->count || lcs[i * width + j + 1] >= lcs[(i + 1) * width + j])) j++;
            else i++;
        }
        const char* data = start < branch->count ? branch->items[start].data : "";
        size_t bytes = 0;
        for (size_t n = start; n < j; n++) bytes += branch->items[n].bytes;
        diff->items[diff->count++] = (struct hunk){first, i, data, bytes};
    }
    u_free(lcs); return 0;
}

static int identical(const struct hunk* a, const struct hunk* b) {
    return a->first == b->first && a->last == b->last && a->bytes == b->bytes && !memcmp(a->data, b->data, a->bytes);
}

static int overlaps(const struct hunk* a, const struct hunk* b) {
    if (a->first == a->last) return a->first >= b->first && a->first <= b->last;
    if (b->first == b->last) return b->first >= a->first && b->first <= a->last;
    return a->first < b->last && b->first < a->last;
}

static int append(struct strbuf* out, const char* data, size_t size) {
    return size && strbuf_addf(out, "%.*s", (int)size, data) < 0 ? -1 : 0;
}

int kanban_text_merge(const char* base, const char* left, const char* right, char** out) {
    if (out) *out = NULL;
    if (!out || !base || !left || !right || strlen(base) > KANBAN_TEXT_MAX ||
        strlen(left) > KANBAN_TEXT_MAX || strlen(right) > KANBAN_TEXT_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "text merge: invalid arguments/size"); return -1;
    }
    const char* simple = !strcmp(left, right) || !strcmp(base, right) ? left : !strcmp(base, left) ? right : NULL;
    if (simple) {
        *out = u_strdup(simple);
        if (!*out) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "text merge: result allocation failed");
        return *out ? 0 : -1;
    }
    struct lines a = {0}, b = {0}, c = {0}; struct diff x = {0}, y = {0}; struct strbuf text = {0}; int result = -1;
    if (split(base, &a) || split(left, &b) || split(right, &c) || difference(&a, &b, &x) || difference(&a, &c, &y)) goto done;
    size_t i = 0, j = 0, cursor = 0;
    while (i < x.count || j < y.count) {
        const struct hunk* h;
        if (i < x.count && j < y.count && overlaps(&x.items[i], &y.items[j])) {
            if (!identical(&x.items[i], &y.items[j])) { result = 1; goto done; }
            h = &x.items[i++]; j++;
        } else if (j == y.count || (i < x.count && x.items[i].first <= y.items[j].first)) h = &x.items[i++];
        else h = &y.items[j++];
        while (cursor < h->first) {
            if (append(&text, a.items[cursor].data, a.items[cursor].bytes)) goto done;
            cursor++;
        }
        if (append(&text, h->data, h->bytes)) goto done;
        cursor = h->last;
    }
    while (cursor < a.count) {
        if (append(&text, a.items[cursor].data, a.items[cursor].bytes)) goto done;
        cursor++;
    }
    *out = u_strdup(strbuf_str(&text)); if (!*out) goto done;
    result = 0;
done:
    if (result < 0) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "text merge: allocation/serialization failed");
    else DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "text merge: result=%s base_bytes=%zu left_bytes=%zu right_bytes=%zu",
        result ? "conflict" : "merged", strlen(base), strlen(left), strlen(right));
    u_free(a.items); u_free(b.items); u_free(c.items); u_free(x.items); u_free(y.items); strbuf_free(&text); return result;
}
