// HTTP headers/body codec: строгие границы сообщений и обработка hop-by-hop полей.
#include "http_proxy.h"
#include "../../lib/debug_config.h"
#include "../../lib/strbuf.h"
#include "../../lib/platform_compat.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>

// Все протокольные ошибки имеют причину; значения credentials в лог не попадают.
static int http_error(int status, const char* reason) {
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "HTTP codec: status=%d reason=%s", status, reason);
    return -status;
}

// ASCII comparison не зависит от locale.
static int ascii_equal(const char* a, size_t len, const char* b) {
    if (len != strlen(b)) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)a[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        unsigned char d = (unsigned char)b[i]; if (d >= 'A' && d <= 'Z') d += 'a' - 'A';
        if (c != d) return 0;
    }
    return 1;
}

// RFC token, включая имена методов и полей.
static int token_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("!#$%&'*+-.^_`|~", c));
}

// Проверяет token list и ищет элемент без учёта регистра.
static int list_contains(const char* value, const char* name) {
    int found = 0;
    while (*value) {
        while (*value == ' ' || *value == '\t' || *value == ',') value++;
        if (!*value) break;
        const char* start = value;
        while (token_char((unsigned char)*value)) value++;
        if (start == value) return -1;
        if (ascii_equal(start, value - start, name)) found = 1;
        while (*value == ' ' || *value == '\t') value++;
        if (*value && *value != ',') return -1;
    }
    return found;
}

// Upgrade содержит protocol-name[/protocol-version], с регистрозависимым сравнением.
static int upgrade_contains(const char* value, const char* selected) {
    int found = 0, count = 0;
    while (*value) {
        while (*value == ' ' || *value == '\t' || *value == ',') value++;
        if (!*value) break;
        const char* start = value;
        while (token_char((unsigned char)*value)) value++;
        if (value == start) return -1;
        if (*value == '/') {
            const char* version = ++value;
            while (token_char((unsigned char)*value)) value++;
            if (version == value) return -1;
        }
        if (selected && strlen(selected) == (size_t)(value - start) && !memcmp(start, selected, value - start)) found = 1;
        count++;
        while (*value == ' ' || *value == '\t') value++;
        if (*value && *value != ',') return -1;
    }
    return count ? found : -1;
}

// Без переполнения и принятия знака/суффикса.
static int parse_number(const char* s, size_t len, unsigned base, uint64_t* result) {
    uint64_t n = 0;
    if (!len) return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                     c >= 'A' && c <= 'F' ? c - 'A' + 10 : base;
        if (d >= base || n > (UINT64_MAX - d) / base) return -1;
        n = n * base + d;
    }
    *result = n; return 0;
}

// Накопление прекращается ровно после CRLFCRLF, независимо от размера recv.
int http_proxy_headers_feed(struct http_proxy_message* m, const uint8_t* data, size_t len, size_t* used) {
    *used = 0;
    while (*used < len && !m->headers_done) {
        if (m->header_len == HTTP_PROXY_HEADER_LIMIT) return http_error(431, "headers limit");
        unsigned char c = data[(*used)++];
        if (!c || c == 127 || (c < 32 && c != '\r' && c != '\n' && c != '\t')) return http_error(400, "header control byte");
        m->headers[m->header_len++] = (char)c;
        size_t n = m->header_len;
        if (c == '\n' && (n < 2 || m->headers[n-2] != '\r')) return http_error(400, "bare LF");
        if (n > 1 && m->headers[n-2] == '\r' && c != '\n') return http_error(400, "bare CR");
        if (!m->first_line_len) {
            if (n > HTTP_PROXY_LINE_LIMIT + 2) return http_error(414, "start line limit");
            if (c == '\n') m->first_line_len = n;
        }
        if (n >= 4 && memcmp(m->headers + n - 4, "\r\n\r\n", 4) == 0) m->headers_done = 1;
    }
    m->headers[m->header_len] = 0;
    return m->headers_done;
}

// Список полей хранит ссылки на собственный проверенный буфер.
static int parse_fields(struct http_proxy_message* m) {
    char* p = m->headers + m->first_line_len;
    m->headers[m->first_line_len - 2] = 0;
    while (*p != '\r') {
        if (m->field_count == HTTP_PROXY_FIELDS_LIMIT) return http_error(431, "field count limit");
        char* end = strstr(p, "\r\n");
        if (!end) return http_error(400, "field terminator");
        char* colon = strchr(p, ':');
        if (!colon || colon >= end || colon == p) return http_error(400, "field name");
        for (char* q = p; q < colon; q++) if (!token_char((unsigned char)*q)) return http_error(400, "field name token");
        *colon = 0; *end = 0;
        char* value = colon + 1; while (*value == ' ' || *value == '\t') value++;
        char* tail = end; while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        m->fields[m->field_count++] = (struct http_proxy_field){p, value};
        p = end + 2;
    }
    unsigned lengths = 0, transfers = 0, hosts = 0;
    for (unsigned i = 0; i < m->field_count; i++) {
        const char* name = m->fields[i].name; const char* value = m->fields[i].value;
        if (ascii_equal(name, strlen(name), "Content-Length")) {
            uint64_t n;
            if (parse_number(value, strlen(value), 10, &n) < 0 || (lengths && n != m->length))
                return http_error(400, "Content-Length");
            m->length = n; m->has_length = 1; lengths++;
        } else if (ascii_equal(name, strlen(name), "Transfer-Encoding")) {
            if (++transfers != 1 || !ascii_equal(value, strlen(value), "chunked") || !m->minor)
                return http_error(400, "unsupported Transfer-Encoding");
            m->chunked = 1;
        } else if (ascii_equal(name, strlen(name), "Connection")) {
            int close = list_contains(value, "close"), upgrade = list_contains(value, "upgrade");
            if (close < 0 || upgrade < 0) return http_error(400, "Connection list");
            m->close |= close; m->upgrade |= upgrade;
            if (list_contains(value, "Content-Length") || list_contains(value, "Transfer-Encoding") || list_contains(value, "Host"))
                return http_error(400, "Connection names framing field");
        } else if (ascii_equal(name, strlen(name), "Host")) {
            if (++hosts != 1 || !*value || strpbrk(value, " \t/@?#,")) return http_error(400, "Host");
        }
    }
    if (m->chunked && m->has_length) return http_error(400, "Transfer-Encoding with Content-Length");
    m->body = m->chunked ? HTTP_BODY_CHUNK_SIZE : m->has_length && m->length ? HTTP_BODY_LENGTH : HTTP_BODY_DONE;
    m->remaining = m->length;
    return 0;
}

// Определяет поля текущего соединения; framing реконструируется отдельно.
static int remove_field(const struct http_proxy_message* m, const char* name) {
    const char* fixed[] = {"Connection", "Proxy-Connection", "Proxy-Authorization", "Proxy-Authenticate",
                          "Keep-Alive", "TE", "Trailer", "Transfer-Encoding", "Content-Length", "Upgrade"};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) if (ascii_equal(name, strlen(name), fixed[i])) return 1;
    for (unsigned i = 0; i < m->field_count; i++)
        if (ascii_equal(m->fields[i].name, strlen(m->fields[i].name), "Connection") && list_contains(m->fields[i].value, name) == 1)
            return 1;
    return 0;
}

// Завершает сериализацию после проверенных полей; исходные байты учитываются независимо от результата.
static int emit_headers(struct http_proxy_message* m, struct strbuf* out, int close, const char* upgrade,
                         http_proxy_emit emit, void* arg) {
    for (unsigned i = 0; i < m->field_count; i++) {
        const char* name = m->fields[i].name;
        if (remove_field(m, name) || (m->status == 0 && ascii_equal(name, strlen(name), "Host"))) continue;
        if (strbuf_addf(out, "%s: %s\r\n", name, m->fields[i].value) < 0) goto alloc_error;
    }
    if (m->has_length && strbuf_addf(out, "Content-Length: %llu\r\n", (unsigned long long)m->length) < 0) goto alloc_error;
    if (m->chunked && !m->decode_chunks && strbuf_addf(out, "Transfer-Encoding: chunked\r\n") < 0) goto alloc_error;
    if (upgrade && *upgrade) {
        if (strbuf_addf(out, "Connection: Upgrade\r\nUpgrade: %s\r\n", upgrade) < 0) goto alloc_error;
    } else if (close && strbuf_addf(out, "Connection: close\r\n") < 0) goto alloc_error;
    if (strbuf_addf(out, "Via: 1.%u utun\r\n\r\n", m->minor) < 0) goto alloc_error;
    if (emit(arg, (const uint8_t*)strbuf_str(out), out->len, m->header_len) < 0) goto alloc_error;
    return 0;
alloc_error:
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "HTTP codec: header output failed bytes=%zu", out->len);
    return -500;
}

// Разбирает authority; IPv6 распознаётся, но общий CONNECT wire содержит только IPv4.
static int parse_authority(const char* s, size_t len, int require_port, struct http_proxy_request* r) {
    if (!len || memchr(s, '@', len) || memchr(s, '#', len)) return http_error(400, "authority");
    if (s[0] == '[') return http_error(502, "IPv6 destination unsupported");
    const char* colon = memchr(s, ':', len);
    size_t host_len = colon ? (size_t)(colon - s) : len;
    if (!host_len || host_len >= sizeof(r->host)) return http_error(400, "host length");
    for (size_t i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return http_error(400, "host syntax");
    }
    r->port = 80;
    if (colon) {
        uint64_t port;
        if (parse_number(colon + 1, len - host_len - 1, 10, &port) < 0 || !port || port > 65535)
            return http_error(400, "port syntax/range");
        r->port = (uint16_t)port;
    } else if (require_port) return http_error(400, "CONNECT port missing");
    memcpy(r->host, s, host_len); r->host[host_len] = 0;
    snprintf(r->authority, sizeof(r->authority), "%s:%u", r->host, r->port);
    return 0;
}

// Строгая request line и origin-form без декодирования path/query.
int http_proxy_request_parse(struct http_proxy_message* m, struct http_proxy_request* r, http_proxy_emit emit, void* arg) {
    char* line = m->headers;
    char* space = memchr(line, ' ', m->first_line_len - 2);
    if (!space || space == line) return http_error(400, "request method");
    for (char* p = line; p < space; p++) if (!token_char((unsigned char)*p)) return http_error(400, "method token");
    char* target = space + 1; char* version = strchr(target, ' ');
    if (!version || version == target) return http_error(400, "request target/version");
    if ((size_t)(m->headers + m->first_line_len - 2 - version) != 9 || memcmp(version, " HTTP/1.", 8) ||
        (version[8] != '0' && version[8] != '1')) return http_error(400, "HTTP version");
    m->minor = version[8] - '0'; *space = 0; *version = 0;
    for (char* p = target; *p; p++) if ((unsigned char)*p <= 32 || *p == '#' || (unsigned char)*p >= 127)
        return http_error(400, "request target byte");
    int ret = parse_fields(m); if (ret < 0) return ret;
    unsigned hosts = 0;
    for (unsigned i = 0; i < m->field_count; i++) if (ascii_equal(m->fields[i].name, strlen(m->fields[i].name), "Host")) hosts++;
    if (m->minor && hosts != 1) return http_error(400, "HTTP/1.1 Host missing");
    r->connect = strcmp(line, "CONNECT") == 0; r->head = strcmp(line, "HEAD") == 0;
    r->minor = m->minor; r->close = m->close || !m->minor;
    if (r->connect) {
        if (m->chunked || m->length) return http_error(400, "CONNECT content");
        return parse_authority(target, strlen(target), 1, r);
    }
    if (strlen(target) < 7 || !ascii_equal(target, 7, "http://")) return http_error(400, "absolute http URL required");
    char* authority = target + 7; char* path = authority + strcspn(authority, "/?");
    ret = parse_authority(authority, path - authority, 0, r); if (ret < 0) return ret;
    if (m->upgrade) {
        if (!m->minor) return http_error(400, "Upgrade requires HTTP/1.1");
        unsigned upgrades = 0;
        for (unsigned i = 0; i < m->field_count; i++) if (ascii_equal(m->fields[i].name, strlen(m->fields[i].name), "Upgrade")) {
            if (++upgrades > 1 || upgrade_contains(m->fields[i].value, NULL) < 0 || strlen(m->fields[i].value) >= sizeof(r->upgrade))
                return http_error(400, "Upgrade value");
            strcpy(r->upgrade, m->fields[i].value);
        }
        if (!upgrades) return http_error(400, "Upgrade missing");
    }
    struct strbuf out = strbuf_new();
    const char* prefix = !*path || *path == '?' ? "/" : "";
    const char* request_path = strcmp(line, "OPTIONS") == 0 && !*path ? "*" : path;
    if (*request_path == '*') prefix = "";
    if (strbuf_addf(&out, "%s %s%s HTTP/1.1\r\nHost: %s\r\n", line, prefix, request_path, r->authority) < 0) ret = -500;
    else ret = emit_headers(m, &out, !m->upgrade, r->upgrade, emit, arg);
    strbuf_free(&out);
    if (ret == -500) DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "HTTP codec: request serialization failed");
    return ret;
}

// Ответы HEAD/1xx/204/304 не имеют тела; Upgrade подтверждается только согласованным 101.
int http_proxy_response_parse(struct http_proxy_message* m, const struct http_proxy_request* r,
                              int close_client, http_proxy_emit emit, void* arg) {
    char* line = m->headers;
    if (m->first_line_len < 15 || memcmp(line, "HTTP/1.", 7) || (line[7] != '0' && line[7] != '1') || line[8] != ' ')
        return http_error(502, "response version/status line");
    uint64_t status;
    if (parse_number(line + 9, 3, 10, &status) < 0 || status < 100 || status > 599 || line[12] != ' ')
        return http_error(502, "response status");
    m->minor = line[7] - '0'; m->status = (unsigned)status;
    int ret = parse_fields(m); if (ret < 0) return http_error(502, "response fields/framing");
    const char* upgrade = NULL;
    if (m->status == 101) {
        if (!r->upgrade[0] || !m->upgrade) return http_error(502, "unsolicited Upgrade");
        for (unsigned i = 0; i < m->field_count; i++) if (ascii_equal(m->fields[i].name, strlen(m->fields[i].name), "Upgrade")) {
            if (upgrade || upgrade_contains(r->upgrade, m->fields[i].value) != 1) return http_error(502, "Upgrade mismatch");
            upgrade = m->fields[i].value;
        }
        if (!upgrade) return http_error(502, "101 Upgrade missing");
    }
    if (r->head || m->status < 200 || m->status == 204 || m->status == 304) m->body = HTTP_BODY_DONE;
    else if (!m->chunked && !m->has_length) { m->body = HTTP_BODY_EOF; close_client = 1; }
    if ((m->status < 200 || m->status == 204) && (m->chunked || m->has_length)) return http_error(502, "body headers on no-content response");
    m->close = close_client;
    m->decode_chunks = !r->minor && m->chunked;
    if (!r->minor && m->status < 200) {
        if (emit(arg, NULL, 0, m->header_len) < 0) return http_error(500, "interim credit output failed");
        return 0;
    }
    struct strbuf out = strbuf_new();
    if (strbuf_addf(&out, "HTTP/1.%u %s\r\n", r->minor, line + 9) < 0) ret = -500;
    else ret = emit_headers(m, &out, close_client, upgrade, emit, arg);
    strbuf_free(&out);
    if (ret == -500) DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "HTTP codec: response serialization failed");
    return ret;
}

// Chunk extensions: tokens/quoted-string с ограниченным размером строки.
static int chunk_size(const char* line, size_t len, uint64_t* size) {
    size_t n = 0;
    while (n < len && ((line[n] >= '0' && line[n] <= '9') || (line[n] >= 'a' && line[n] <= 'f') ||
                      (line[n] >= 'A' && line[n] <= 'F'))) n++;
    if (parse_number(line, n, 16, size) < 0) return -1;
    while (n < len) {
        while (n < len && (line[n] == ' ' || line[n] == '\t')) n++;
        if (n == len || line[n++] != ';') return -1;
        while (n < len && (line[n] == ' ' || line[n] == '\t')) n++;
        size_t start = n; while (n < len && token_char((unsigned char)line[n])) n++;
        if (start == n) return -1;
        while (n < len && (line[n] == ' ' || line[n] == '\t')) n++;
        if (n < len && line[n] == '=') {
            n++; while (n < len && (line[n] == ' ' || line[n] == '\t')) n++;
            if (n < len && line[n] == '"') {
                n++;
                while (n < len && line[n] != '"') {
                    if (line[n] == '\\') { if (++n == len) return -1; }
                    if ((unsigned char)line[n] < 32 && line[n] != '\t') return -1;
                    n++;
                }
                if (n == len) return -1;
                n++;
            } else {
                start = n; while (n < len && token_char((unsigned char)line[n])) n++;
                if (start == n) return -1;
            }
        }
    }
    return 0;
}

// Тело не буферизуется целиком; только chunk size/trailer line ожидают полного CRLF.
int http_proxy_body_feed(struct http_proxy_message* m, const uint8_t* data, size_t len, size_t* used,
                         http_proxy_emit emit, void* arg) {
    *used = 0;
    while (*used < len && m->body != HTTP_BODY_DONE) {
        if (m->body == HTTP_BODY_LENGTH || m->body == HTTP_BODY_CHUNK_DATA || m->body == HTTP_BODY_EOF) {
            size_t n = len - *used;
            if (m->body != HTTP_BODY_EOF && m->remaining < n) n = (size_t)m->remaining;
            if (emit(arg, data + *used, n, n) < 0) return http_error(500, "body output failed");
            *used += n;
            if (m->body != HTTP_BODY_EOF) {
                m->remaining -= n;
                if (!m->remaining) m->body = m->body == HTTP_BODY_LENGTH ? HTTP_BODY_DONE : HTTP_BODY_CHUNK_CRLF;
            }
        } else if (m->body == HTTP_BODY_CHUNK_CRLF) {
            unsigned char c = data[(*used)++];
            if (c != (m->crlf_pos ? '\n' : '\r')) return http_error(400, "chunk data CRLF");
            if (++m->crlf_pos == 2) {
                if (emit(arg, (const uint8_t*)"\r\n", m->decode_chunks ? 0 : 2, 2) < 0)
                    return http_error(500, "chunk CRLF output failed");
                m->crlf_pos = 0; m->body = HTTP_BODY_CHUNK_SIZE;
            }
        } else {
            if (m->line_len == HTTP_PROXY_LINE_LIMIT) return http_error(431, "chunk/trailer line limit");
            unsigned char c = data[(*used)++];
            if (!c || c == 127 || (c < 32 && c != '\r' && c != '\n' && c != '\t')) return http_error(400, "chunk/trailer control byte");
            m->line[m->line_len++] = (char)c;
            size_t n = m->line_len;
            if (c == '\n' && (n < 2 || m->line[n-2] != '\r')) return http_error(400, "chunk/trailer bare LF");
            if (n > 1 && m->line[n-2] == '\r' && c != '\n') return http_error(400, "chunk/trailer bare CR");
            if (c != '\n') continue;
            m->line[n] = 0;
            int drop = m->decode_chunks;
            if (m->body == HTTP_BODY_CHUNK_SIZE) {
                if (chunk_size(m->line, n - 2, &m->remaining) < 0) return http_error(400, "chunk size/extensions");
                m->body = m->remaining ? HTTP_BODY_CHUNK_DATA : HTTP_BODY_TRAILERS;
            } else {
                m->trailer_len += n;
                if (m->trailer_len > HTTP_PROXY_HEADER_LIMIT) return http_error(431, "trailers limit");
                if (n == 2) m->body = HTTP_BODY_DONE;
                else {
                    char* colon = memchr(m->line, ':', n - 2);
                    if (!colon || colon == m->line) return http_error(400, "trailer name");
                    for (char* p = m->line; p < colon; p++) if (!token_char((unsigned char)*p)) return http_error(400, "trailer token");
                    *colon = 0;
                    if (ascii_equal(m->line, strlen(m->line), "Host")) return http_error(400, "Host trailer");
                    if (ascii_equal(m->line, strlen(m->line), "Content-Length") ||
                        ascii_equal(m->line, strlen(m->line), "Transfer-Encoding")) return http_error(400, "framing trailer");
                    drop |= remove_field(m, m->line); *colon = ':';
                }
            }
            if (emit(arg, (const uint8_t*)m->line, drop ? 0 : n, n) < 0) return http_error(500, "chunk/trailer output failed");
            m->line_len = 0;
        }
    }
    return m->body == HTTP_BODY_DONE;
}
