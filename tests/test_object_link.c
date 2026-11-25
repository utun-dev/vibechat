/* Постоянные ссылки: общий формат, Unicode, экранирование и атомарный отказ разбора. */
#include "chat/object_link.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void reject(const char* url) {
    struct object_link_target out, before; memset(&out, 0xa5, sizeof(out)); before = out;
    char error[80];
    assert(object_link_parse(url, strlen(url), &out, error, sizeof(error)) < 0);
    assert(*error && !memcmp(&out, &before, sizeof(out)));
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG);
    struct object_link_target input = {0}, output;
    memset(input.group, 'c', 64);
    memset(input.card, 'a', 64); memset(input.message, 'b', 64);
    char url[OBJECT_LINK_MAX + 1], changed[OBJECT_LINK_MAX + 1];
    for (int kind = OBJECT_LINK_CHAT_MESSAGE; kind <= OBJECT_LINK_CARD_MESSAGE; kind++) {
        input.kind = kind;
        int size = object_link_build(&input, " Команда  разработки ", "Исправить / вход@?#%", "Проверил 😀\nAndroid", url, sizeof(url));
        assert(size > 0 && strstr(url, "Команда-разработки@") && strstr(url, input.group));
        assert(!object_link_parse(url, size, &output, NULL, 0));
        assert(output.kind == kind && !strcmp(output.group, input.group));
        if (kind != OBJECT_LINK_CHAT_MESSAGE) assert(!strcmp(output.card, input.card) && strstr(url, "%2F") && strstr(url, "%40"));
        if (kind != OBJECT_LINK_CARD) assert(!strcmp(output.message, input.message) && strstr(url, "Проверил-😀-Android"));
        assert(object_link_build(&input, "другая подпись", "карточка", "текст изменён", changed, sizeof(changed)) > 0);
        struct object_link_target same; assert(!object_link_parse(changed, strlen(changed), &same, NULL, 0));
        assert(!memcmp(&same, &output, sizeof(same)));
        reject(strcat(strcpy(changed, url), "/"));
        reject(strcat(strcpy(changed, url), "/extra"));
        reject(strcat(strcpy(changed, url), "?extra"));
        assert(object_link_build(&input, NULL, NULL, NULL, changed, sizeof(changed)) > 0);
        assert(!object_link_parse(changed, strlen(changed), &same, NULL, 0));
        assert(!memcmp(&same, &output, sizeof(same)));
        assert(object_link_build(&input, NULL, NULL, NULL, changed, 4) < 0);
    }
    snprintf(url, sizeof(url), "utun://chat/%%D0%%A2%%2F%%40@%s/%%F0%%9F%%98%%80@%s", input.group, input.message);
    assert(!object_link_parse(url, strlen(url), &output, NULL, 0));
    snprintf(url, sizeof(url), "utun://chat/a@%s/@%064d", input.group, 0);
    assert(!object_link_parse(url, strlen(url), &output, NULL, 0));
    for (const char** p = (const char*[]){"utun://", "utun://chat/", "utun://chat/@1", "utun://chat/@0/@a",
        "utun://chat/@01/@a", "utun://chat/@9223372036854775808/@a", "utun://kanban/@1/@bad", NULL}; *p; p++) reject(*p);
    snprintf(url, sizeof(url), "utun://chat/%%GG@%s/@%s", input.group, input.message); reject(url);
    snprintf(url, sizeof(url), "utun://chat/%%00@%s/@%s", input.group, input.message); reject(url);
    snprintf(url, sizeof(url), "utun://chat/%%C0%%AF@%s/@%s", input.group, input.message); reject(url);
    snprintf(url, sizeof(url), "utun://chat/space here@%s/@%s", input.group, input.message); reject(url);
    snprintf(url, sizeof(url), "utun://chat/@%s/@%s", input.group, input.message); url[strlen(url) - 1] = 'Z'; reject(url);
    puts("object links: roundtrip, renamed labels, Unicode, escaping and malformed URLs passed"); return 0;
}
