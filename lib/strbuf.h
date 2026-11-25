/*
 * strbuf.h — safe growable printf buffer
 *
 * Replaces snprintf(buf+off, sizeof(buf)-off, ...) chains that crash
 * on overflow (snprintf returns un-truncated size, offset exceeds buffer,
 * next call gets negative size → FORTIFY abort on Bionic/Android).
 *
 * Usage:
 *   struct strbuf sb = strbuf_new();
 *   strbuf_addf(&sb, "{\"ver\":\"%d\"", ver);
 *   strbuf_addf(&sb, ",\"name\":\"%s\"", name);
 *   strbuf_addf(&sb, "}");
 *   // use strbuf_str(&sb) or strbuf_detach(&sb)
 *   strbuf_free(&sb);
 */
#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>
#include <stdarg.h>

struct strbuf {
    char  *buf;      /* allocated buffer, always null-terminated */
    size_t cap;      /* total capacity (including '\0') */
    size_t len;      /* current length (excluding '\0') */
};

/* Размер ограничен UINT32_MAX allocator-ом mem.h;превышение отклоняется до преобразования size_t.
 * init предназначен для нового буфера;повторная инициализация живого буфера требует предварительного free. */

/* zero-init on stack: struct strbuf sb = {0}; */
#define strbuf_new()  ((struct strbuf){NULL, 0, 0})

void   strbuf_init(struct strbuf *sb, size_t initial_cap);
void   strbuf_free(struct strbuf *sb);

/* returns buf and detaches — caller must u_free(). sb is reset to empty */
char  *strbuf_detach(struct strbuf *sb);

/* reset len=0, keep allocation */
void   strbuf_reset(struct strbuf *sb);

/* printf-добавление с ростом буфера: число байтов без NUL или < 0 при ошибке форматирования/роста. */
int    strbuf_addf(struct strbuf *sb, const char *fmt, ...)
       __attribute__((format(printf, 2, 3)));

/* Добавить JSON-строку с кавычками и полным экранированием; NULL означает пустую строку. 0/-1. */
int    strbuf_add_json_string(struct strbuf* sb, const char* text);

void   strbuf_addc(struct strbuf *sb, char c);
void   strbuf_adds(struct strbuf *sb, const char *s);

/* safe accessor — never returns NULL */
#define strbuf_str(sb)  ((sb)->buf ? (sb)->buf : "")

#endif /* STRBUF_H */
