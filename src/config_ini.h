/* Внутренний разбор INI для config_parser и config_updater: одинаковые пробелы,
 * секции, key=value и строгий HEX. Буфер принадлежит вызывающему; trim/key_value
 * меняют его на месте. read выделяет текст через u_malloc; освобождение — u_free.
 * Строка содержит не более 1022 байтов до LF, встроенный NUL запрещён. */
#ifndef CONFIG_INI_H
#define CONFIG_INI_H

#include "config_error.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/strbuf.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_INI_LINE_SIZE 1024

/* Удалить пробелы с обеих сторон, сохранив исходный адрес строки. */
static inline char *config_ini_trim(char *text) {
    if (!text) return NULL;
    char *start = text;
    while (isspace((unsigned char)*start)) start++;
    size_t length = strlen(start);
    while (length && isspace((unsigned char)start[length - 1])) length--;
    memmove(text, start, length);
    text[length] = '\0';
    return text;
}

/* Полный заголовок [name], без префиксного сопоставления; 0/-1. */
static inline int config_ini_section(const char *line, char *name, size_t capacity) {
    size_t length = strlen(line);
    if (length < 2 || line[0] != '[' || line[length - 1] != ']' || length - 2 >= capacity) return -1;
    memcpy(name, line + 1, length - 2);
    name[length - 2] = '\0';
    config_ini_trim(name);
    return 0;
}

/* Разобрать key=value, обрезать пробелы и комментарий # в значении; 0/-1. */
static inline int config_ini_key_value(const char *line, char *key, size_t key_cap, char *value, size_t value_cap) {
    const char *equal = strchr(line, '=');
    if (!equal || (size_t)(equal - line) >= key_cap || strlen(equal + 1) >= value_cap) return -1;
    memcpy(key, line, (size_t)(equal - line));
    key[equal - line] = '\0';
    config_ini_trim(key);
    strcpy(value, equal + 1);
    char *comment = strchr(value, '#');
    if (comment) *comment = '\0';
    config_ini_trim(value);
    return key[0] ? 0 : -1;
}

/* Проверить физические строки до разбора, чтобы не принять хвост длинной строки за новую опцию. */
static inline int config_ini_validate(const char *text, size_t length, const char *filename) {
    size_t column = 0, line = 1;
    if (!text && length) { CONFIG_ERROR("%s: NULL configuration buffer", filename); errno = EINVAL; return -1; }
    for (size_t i = 0; i < length; i++) {
        if (!text[i]) {
            config_error_set_location((int)line, NULL);
            CONFIG_ERROR("%s:%zu: embedded NUL at byte %zu", filename, line, i);
            errno = EINVAL;
            return -1;
        }
        if (text[i] != '\n' && ++column >= CONFIG_INI_LINE_SIZE - 1) {
            config_error_set_location((int)line, NULL);
            CONFIG_ERROR("%s:%zu: INI line exceeds 1022 bytes", filename, line);
            errno = EOVERFLOW;
            return -1;
        }
        if (text[i] == '\n') { column = 0; line++; }
    }
    return 0;
}

/* Прочитать весь поток, проверяя ошибки чтения/роста. Поток закрывает вызывающий; 0/-1. */
static inline int config_ini_read(FILE *file, const char *filename, char **text, size_t *length) {
    struct strbuf output = strbuf_new();
    char chunk[4096];
    size_t count;
    *text = NULL;
    *length = 0;
    while ((count = fread(chunk, 1, sizeof(chunk), file)) != 0) {
        const char *nul = (const char *)memchr(chunk, '\0', count);
        if (nul) {
            CONFIG_ERROR("%s: embedded NUL at byte %zu", filename, output.len + (size_t)(nul - chunk));
            errno = EINVAL;
            goto failed;
        }
        if (output.len > UINT32_MAX / 2 - count - 1) {
            CONFIG_ERROR("%s: INI text exceeds allocator size limit", filename);
            errno = EFBIG;
            goto failed;
        }
        if (strbuf_addf(&output, "%.*s", (int)count, chunk) < 0) {
            CONFIG_ERROR("%s: allocation failed while reading INI at byte %zu", filename, output.len);
            errno = ENOMEM;
            goto failed;
        }
    }
    if (ferror(file)) {
        CONFIG_ERROR("%s: read failed at byte %zu: %s", filename, output.len, strerror(errno));
        goto failed;
    }
    if (config_ini_validate(strbuf_str(&output), output.len, filename)) goto failed;
    if (!output.buf && strbuf_addf(&output, "%s", "") < 0) {
        CONFIG_ERROR("%s: cannot allocate empty configuration buffer", filename);
        errno = ENOMEM;
        goto failed;
    }
    *length = output.len;
    *text = strbuf_detach(&output);
    return 0;
failed:
    strbuf_free(&output);
    return -1;
}

static inline int config_ini_nibble(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Проверить весь HEX-фрагмент до записи. binary=NULL — только проверка; 0/-1. */
static inline int config_ini_hex_span(const char *text, size_t length, uint8_t *binary, size_t size) {
    if (!text || size > SIZE_MAX / 2 || length != size * 2) return -1;
    for (size_t i = 0; i < length; i++) if (config_ini_nibble((unsigned char)text[i]) < 0) return -1;
    if (binary) {
        for (size_t i = 0; i < size; i++) {
            binary[i] = (uint8_t)((config_ini_nibble((unsigned char)text[i * 2]) << 4) |
                                 config_ini_nibble((unsigned char)text[i * 2 + 1]));
        }
    }
    return 0;
}

static inline int config_ini_hex(const char *text, uint8_t *binary, size_t size) {
    return text ? config_ini_hex_span(text, strlen(text), binary, size) : -1;
}

/* Список HEX через запятую: пустой допустим; непустые элементы имеют item_size байтов.
 * Только полностью корректный список копируется в binary; при ошибке *count=0. */
static inline int config_ini_hex_list(const char *text, uint8_t *binary, size_t item_size, int max_count, int *count) {
    *count = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *cursor = text;
        int items = 0;
        if (!*cursor) return 0;
        do {
            const char *end = strchr(cursor, ',');
            const char *next = end ? end + 1 : NULL;
            if (!end) end = cursor + strlen(cursor);
            while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
            while (end > cursor && isspace((unsigned char)end[-1])) end--;
            if (items >= max_count || config_ini_hex_span(cursor, (size_t)(end - cursor),
                pass && binary ? binary + (size_t)items * item_size : NULL, item_size)) return -1;
            items++;
            cursor = next;
        } while (cursor);
        if (pass) *count = items;
    }
    return 0;
}

#endif
