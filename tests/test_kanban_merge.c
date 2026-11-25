/* Слияние в том же C-модуле, который используют Qt/JNI; границы строк и потери данных. */
#include "../src/kanban/text_merge.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void merge(const char* base, const char* left, const char* right, const char* expected) {
    char* result = NULL;
    assert(kanban_text_merge(base, left, right, &result) == (expected ? 0 : 1));
    if (expected) assert(result && !strcmp(result, expected));
    else assert(!result);
    u_free(result);
    assert(kanban_text_merge(base, right, left, &result) == (expected ? 0 : 1));
    if (expected) assert(result && !strcmp(result, expected));
    else assert(!result);
    u_free(result);
}

int main(void) {
    debug_set_level(DEBUG_LEVEL_WARN);
    merge("a\nb\nc\n", "A\nb\nc\n", "a\nb\nC\n", "A\nb\nC\n");
    merge("a\nb\nc\n", "a\nB\nc\n", "a\nB\nc\n", "a\nB\nc\n");
    merge("a\nb\n", "a\nB\n", "a\nC\n", NULL);
    merge("a\nb\n", "a\nx\nb\n", "a\ny\nb\n", NULL);
    merge("a\nb\nc\nd\n", "a\nc\nd\n", "a\nb\nc\nD\n", "a\nc\nD\n");
    merge("a\nb\nc\n", "a\nx\nb\nc\n", "a\nb\nc\ny\n", "a\nx\nb\nc\ny\n");
    merge("первая\nвторая\nтретья", "ПЕРВАЯ\nвторая\nтретья", "первая\nвторая\nТРЕТЬЯ", "ПЕРВАЯ\nвторая\nТРЕТЬЯ");
    merge("", "first", "second", NULL);
    merge("a", "", "a", "");
    merge("a\nb\n", "A\nb\n", "a\nB\n", "A\nB\n");
    merge("one\ntwo", "one\ntwo\n", "one\ntwo", "one\ntwo\n");
    assert(!u_get_allocated_count());
    puts("PASS kanban merge: disjoint edits, insert/delete, Unicode, conflicts, newline and branch order");
    return 0;
}
