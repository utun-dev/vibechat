/*
 * async_dns.c — асинхронный (неблокирующий) DNS-резолвер поверх uasync.
 *
 * Адаптер над libdns (lib/dns.c). Схема работы:
 *   adns_resolve() → dns_res_submit(A-запрос) → adns_drive()
 *   adns_drive()  → dns_res_check():
 *       0      → готово, dns_res_fetch → разбор A-записей → коллбэк
 *       EAGAIN → зарегистрировать fd (dns_res_pollfd/events) + таймер
 *                (dns_res_timeout) в uasync и ждать события
 *       иначе  → ошибка → коллбэк с ADNS_ERR_RESPONSE
 *
 * Всё выполняется в потоке event loop, блокирующих вызовов нет.
 */
#include "async_dns.h"
#include "dns.h"
#include "debug_config.h"
#include "mem.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32
#include <iphlpapi.h>
#endif

#define ADNS_DEBUG_CAT DEBUG_CATEGORY_SOCKET

struct adns_query {
    struct UASYNC* ua;
    adns_done_cb cb;
    void* arg;
    struct dns_resolver* res;
    void* socket_id;    /* handle uasync (NULL когда не зарегистрирован) */
    int   cur_fd;       /* текущий зарегистрированный fd (-1 когда нет) */
    void* timer;        /* handle таймаута (NULL когда нет) */
    int   finished;
    char* name;
    int   fast;                  /* 1 = готовый /etc/hosts-ответ, без libdns */
    struct adns_result fast_result;
    void* call_soon_id;          /* отложенный вызов (fast-путь и первый драйв slow-пути) */
};

static void adns_drive(struct adns_query* q);
static void adns_finish(struct adns_query* q, int status, struct sockaddr_in* addrs, int count);
static void adns_read_cb(socket_t sock, void* arg);
static void adns_write_cb(socket_t sock, void* arg);
static void adns_timer_cb(void* arg);
static void adns_fast_ready_cb(void* arg);
static int  adns_hosts_lookup(const char* name, struct sockaddr_in* out);

/* ─── system DNS servers discovery ─── */

int adns_parse_resolv_conf(const char* text, struct sockaddr_in* out, int max) {
    int n = 0;
    const char* p = text;

    if (!text || !out || max <= 0) return 0;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        if (strncmp(p, "nameserver", 10) == 0 && (p[10] == ' ' || p[10] == '\t')) {
            p += 10;
            while (*p == ' ' || *p == '\t') p++;
            char tok[64];
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '#' && i < 63)
                tok[i++] = *p++;
            tok[i] = '\0';
            memset(&out[n], 0, sizeof(out[n]));
            if (tok[0] && inet_pton(AF_INET, tok, &out[n].sin_addr) == 1) {
                out[n].sin_family = AF_INET;
                out[n].sin_port = htons(53);
                n++;
            }
        }

        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return n;
}

#ifdef _WIN32
int adns_system_servers(struct sockaddr_in* out, int max) {
    if (!out || max <= 0) return 0;

    ULONG size = 0;
    if (GetNetworkParams(NULL, &size) != ERROR_BUFFER_OVERFLOW) return 0;
    PFIXED_INFO fi = (PFIXED_INFO)u_malloc(size);
    if (!fi) return 0;
    if (GetNetworkParams(fi, &size) != NO_ERROR) { u_free(fi); return 0; }

    int n = 0;
    IP_ADDR_STRING* p = &fi->DnsServerList;
    while (p && n < max) {
        const char* s = p->IpAddress.String;
        while (*s && n < max) {
            while (*s == ' ' || *s == ',') s++;
            char tok[64];
            int i = 0;
            while (*s && *s != ' ' && *s != ',' && i < 63) tok[i++] = *s++;
            tok[i] = '\0';
            memset(&out[n], 0, sizeof(out[n]));
            if (tok[0] && inet_pton(AF_INET, tok, &out[n].sin_addr) == 1) {
                out[n].sin_family = AF_INET;
                out[n].sin_port = htons(53);
                n++;
            }
        }
        p = p->Next;
    }
    u_free(fi);
    return n;
}
#else
int adns_system_servers(struct sockaddr_in* out, int max) {
    if (!out || max <= 0) return 0;
    FILE* f = fopen("/etc/resolv.conf", "r");
    if (!f) return 0;
    char buf[4096];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    return adns_parse_resolv_conf(buf, out, max);
}
#endif

/* ─── синхронный /etc/hosts-лукап (быстрый путь, без libdns) ─── */

// Ищет name в /etc/hosts. Возвращает 1 если найден IPv4-адрес (out заполнен).
// Блокирующее чтение небольшого файла — только для локальных имён (localhost и т.п.).
static int adns_hosts_lookup(const char* name, struct sockaddr_in* out) {
#ifndef _WIN32
    if (!name || !out) return 0;
    FILE* f = fopen("/etc/hosts", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        char ip[64]; int n = 0;
        const char* p = line;
        while (*p && !isspace(*p) && n < 63) ip[n++] = *p++;
        ip[n] = '\0';
        if (!ip[0] || ip[0] == '#') continue;
        struct in_addr a;
        if (inet_pton(AF_INET, ip, &a) != 1) continue;
        while (*p && isspace(*p)) p++;
        while (*p && *p != '\n') {
            if (*p == '#') break;
            char h[256]; int hn = 0;
            while (*p && !isspace(*p) && *p != '#' && hn < 255) h[hn++] = *p++;
            h[hn] = '\0';
            if (h[0] && strcasecmp(h, name) == 0) {
                memset(out, 0, sizeof(*out));
                out->sin_family = AF_INET;
                out->sin_addr = a;
                found = 1;
                break;
            }
            while (*p && isspace(*p)) p++;
        }
    }
    fclose(f);
    return found;
#else
    (void)name; (void)out;
    return 0;
#endif
}

/* ─── resolver setup ─── */

static struct dns_resolver* adns_open_resolver(const struct sockaddr_in* servers, int count,
                                               int timeout_ms, int attempts) {
    int error = 0;
    struct dns_resolv_conf* rc = dns_resconf_open(&error);
    if (!rc) {
        DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: dns_resconf_open failed: %s", dns_strerror(error));
        return NULL;
    }

    for (int i = 0; i < count && i < 3; i++) {
        struct sockaddr_storage* ss = &rc->nameserver[i];
        struct sockaddr_in* sin = (struct sockaddr_in*)ss;
        memset(ss, 0, sizeof(*ss));
        sin->sin_family = AF_INET;
        sin->sin_addr = servers[i].sin_addr;
        sin->sin_port = servers[i].sin_port ? servers[i].sin_port : htons(53);
    }
    if (timeout_ms > 0) {
        unsigned sec = (unsigned)((timeout_ms + 999) / 1000);
        rc->options.timeout = sec ? sec : 1;
    }
    if (attempts > 0) rc->options.attempts = (unsigned)attempts;

    struct dns_hosts* hosts = dns_hosts_open(&error);
    if (!hosts) {
        DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: dns_hosts_open failed: %s", dns_strerror(error));
        dns_resconf_close(rc);
        return NULL;
    }
#ifndef _WIN32
    // Локальные имена из /etc/hosts (localhost и т.п.). Не фатально, если файла нет.
    if (dns_hosts_loadpath(hosts, "/etc/hosts") != 0)
        DEBUG_DEBUG(ADNS_DEBUG_CAT, "adns: /etc/hosts not loaded");
#endif

    struct dns_hints* hints = dns_hints_local(rc, &error);
    if (!hints) {
        DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: dns_hints_local failed: %s", dns_strerror(error));
        dns_hosts_close(hosts);
        dns_resconf_close(rc);
        return NULL;
    }

    struct dns_resolver* res = dns_res_open(rc, hosts, hints, NULL, NULL, &error);
    dns_resconf_close(rc);
    dns_hosts_close(hosts);
    dns_hints_close(hints);

    if (!res)
        DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: dns_res_open failed: %s", dns_strerror(error));
    return res;
}

/* ─── driving state machine ─── */

static void adns_teardown(struct adns_query* q) {
    if (q->timer) { uasync_cancel_timeout(q->ua, q->timer); q->timer = NULL; }
    if (q->socket_id) {
        uasync_remove_socket_t(q->ua, (socket_t)q->cur_fd);
        q->socket_id = NULL;
    }
    q->cur_fd = -1;
    if (q->res) { dns_res_close(q->res); q->res = NULL; }
}

static void adns_rearm(struct adns_query* q) {
    int fd = dns_res_pollfd(q->res);
    int events = dns_res_events(q->res);

    if (fd != q->cur_fd) {
        if (q->socket_id) {
            uasync_remove_socket_t(q->ua, (socket_t)q->cur_fd);
            q->socket_id = NULL;
        }
        q->cur_fd = -1;
        if (fd >= 0) {
            q->socket_id = uasync_add_socket_t(q->ua, (socket_t)fd, adns_read_cb, adns_write_cb, NULL, "dns_query", q);
            if (q->socket_id) q->cur_fd = fd;
            else DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: uasync_add_socket_t(%d) failed", fd);
        }
    }

    if (q->socket_id) {
        uasync_set_socket_read(q->ua, q->socket_id, (events & DNS_POLLIN) ? 1 : 0);
        uasync_set_socket_write(q->ua, q->socket_id, (events & DNS_POLLOUT) ? 1 : 0);
    }

    time_t t = dns_res_timeout(q->res);
    if (t < 1) t = 1;
    if (q->timer) { uasync_cancel_timeout(q->ua, q->timer); q->timer = NULL; }
    q->timer = uasync_set_timeout(q->ua, (int)(t * 10000), q, adns_timer_cb, "adns");
    if (!q->timer)
        DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: uasync_set_timeout failed");
}

static void adns_drive(struct adns_query* q) {
    if (q->finished) return;

    int error = dns_res_check(q->res);
    if (error == 0) {
        int ferr = 0;
        struct dns_packet* answer = dns_res_fetch(q->res, &ferr);
        if (!answer) {
            DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: '%s' dns_res_fetch failed: %s", q->name, dns_strerror(ferr));
            adns_finish(q, ADNS_ERR_RESPONSE, NULL, 0);
            return;
        }

        unsigned rcode = dns_p_rcode(answer);
        if (rcode == DNS_RC_NOERROR) {
            struct sockaddr_in addrs[ADNS_MAX_ADDRS];
            int n = 0;
            struct dns_rr rr;
            struct dns_a a;
            dns_rr_foreach(&rr, answer, .section = DNS_S_AN, .type = DNS_T_A) {
                if (n >= ADNS_MAX_ADDRS) break;
                dns_a_parse(&a, &rr, answer);
                memset(&addrs[n], 0, sizeof(addrs[n]));
                addrs[n].sin_family = AF_INET;
                addrs[n].sin_addr = a.addr;
                n++;
            }
            free(answer);
            if (n > 0) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addrs[0].sin_addr, ip, sizeof(ip));
                DEBUG_INFO(ADNS_DEBUG_CAT, "adns: '%s' resolved to %d A record(s), first=%s", q->name, n, ip);
                adns_finish(q, ADNS_OK, addrs, n);
            } else {
                DEBUG_WARN(ADNS_DEBUG_CAT, "adns: '%s' no A records (NOERROR, empty answer)", q->name);
                adns_finish(q, ADNS_ERR_NODATA, NULL, 0);
            }
        } else {
            const char* rc = dns_strrcode((enum dns_rcode)rcode);
            if (rcode == DNS_RC_NXDOMAIN) {
                free(answer);
                DEBUG_WARN(ADNS_DEBUG_CAT, "adns: '%s' NXDOMAIN", q->name);
                adns_finish(q, ADNS_ERR_NODATA, NULL, 0);
            } else if (rcode == DNS_RC_SERVFAIL) {
                /* libdns синтезирует SERVFAIL и при таймауте (ответа не получено) */
                const struct dns_stat* st = dns_res_stat(q->res);
                int no_resp = st && st->udp.rcvd.count == 0;
                free(answer);
                if (no_resp) {
                    DEBUG_WARN(ADNS_DEBUG_CAT, "adns: '%s' timed out (no response from nameservers)", q->name);
                    adns_finish(q, ADNS_ERR_TIMEOUT, NULL, 0);
                } else {
                    DEBUG_WARN(ADNS_DEBUG_CAT, "adns: '%s' SERVFAIL", q->name);
                    adns_finish(q, ADNS_ERR_RESPONSE, NULL, 0);
                }
            } else {
                free(answer);
                DEBUG_WARN(ADNS_DEBUG_CAT, "adns: '%s' rcode=%s — response error", q->name, rc);
                adns_finish(q, ADNS_ERR_RESPONSE, NULL, 0);
            }
        }
        return;
    }

    if (error == EAGAIN) {
        adns_rearm(q);
        return;
    }

    DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: '%s' dns_res_check error=%d (%s)", q->name, error, dns_strerror(error));
    adns_finish(q, ADNS_ERR_RESPONSE, NULL, 0);
}

static void adns_read_cb(socket_t sock, void* arg) {
    (void)sock;
    adns_drive((struct adns_query*)arg);
}

static void adns_write_cb(socket_t sock, void* arg) {
    (void)sock;
    adns_drive((struct adns_query*)arg);
}

static void adns_timer_cb(void* arg) {
    struct adns_query* q = (struct adns_query*)arg;
    q->timer = NULL;
    adns_drive(q);
}

static void adns_finish(struct adns_query* q, int status, struct sockaddr_in* addrs, int count) {
    if (q->finished) return;
    q->finished = 1;

    adns_teardown(q);

    struct adns_result res;
    res.status = status;
    res.count = count > 0 ? count : 0;
    if (res.count > 0 && addrs) memcpy(res.addrs, addrs, (size_t)res.count * sizeof(*addrs));

    adns_done_cb cb = q->cb;
    void* arg = q->arg;
    char* name = q->name;
    q->name = NULL;
    u_free(q);

    if (cb) cb(&res, arg);
    u_free(name);
}

// Отложенный вызов коллбэка для fast-пути (/etc/hosts): сохраняет async-контракт
// (adns_resolve возвращает handle, коллбэк вызывается в потоке event loop).
static void adns_fast_ready_cb(void* arg) {
    struct adns_query* q = (struct adns_query*)arg;
    q->call_soon_id = NULL;
    if (q->finished) return;   // отменён до срабатывания
    q->finished = 1;
    adns_done_cb cb = q->cb;
    void* a = q->arg;
    struct adns_result res = q->fast_result;
    char* nm = q->name;
    q->name = NULL;
    u_free(q);
    if (cb) cb(&res, a);
    u_free(nm);
}

// Отложенный первый драйв slow-пути (libdns): adns_resolve возвращает живой handle,
// а реальный запуск запроса происходит в event loop. Это исключает ситуацию, когда
// dns_res_check завершается синхронно прямо внутри adns_resolve() и освобождает q
// до того, как вызывающий код получил и сохранил handle.
static void adns_drive_trampoline(void* arg) {
    struct adns_query* q = (struct adns_query*)arg;
    q->call_soon_id = NULL;
    if (q->finished) return;   // отменён до срабатывания
    adns_drive(q);
}

/* ─── public API ─── */

struct adns_query* adns_resolve(struct UASYNC* ua, const char* name,
                                const struct adns_opts* opts,
                                adns_done_cb cb, void* arg) {
    if (!ua || !name || !name[0] || !cb) return NULL;
    if (strlen(name) > 255) { DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: name too long"); return NULL; }

    // Быстрый путь: имя из /etc/hosts (localhost и т.п.) — без libdns и сети.
    // libdns по умолчанию идёт в DNS первым (lookup="bf"), поэтому локальные имена
    // ждали бы таймаут DNS-запроса (2×2s); обрабатываем их сразу.
    {
        struct sockaddr_in ha;
        if (adns_hosts_lookup(name, &ha)) {
            struct adns_query* q = u_calloc(1, sizeof(*q));
            if (!q) { DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: calloc failed"); return NULL; }
            q->ua = ua; q->cb = cb; q->arg = arg;
            q->name = u_strdup(name);
            if (!q->name) { u_free(q); DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: strdup failed"); return NULL; }
            q->fast = 1;
            q->fast_result.status = ADNS_OK;
            q->fast_result.count = 1;
            q->fast_result.addrs[0] = ha;
            q->call_soon_id = uasync_call_soon(ua, q, adns_fast_ready_cb);
            if (!q->call_soon_id) { u_free(q->name); u_free(q); return NULL; }
            DEBUG_DEBUG(ADNS_DEBUG_CAT, "adns: '%s' resolved from /etc/hosts (fast)", name);
            return q;
        }
    }

    struct sockaddr_in servers[3];
    int count;
    if (opts && opts->server.sin_family != 0) {
        servers[0] = opts->server;
        count = 1;
    } else {
        count = adns_system_servers(servers, 3);
    }
    if (count <= 0) {
        DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: no DNS servers available for '%s'", name);
        return NULL;
    }

    int timeout_ms = (opts && opts->timeout_ms > 0) ? opts->timeout_ms : 2000;
    int attempts = (opts && opts->max_attempts > 0) ? opts->max_attempts : 2;

    struct adns_query* q = u_calloc(1, sizeof(*q));
    if (!q) { DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: calloc failed"); return NULL; }
    q->ua = ua;
    q->cb = cb;
    q->arg = arg;
    q->cur_fd = -1;
    q->name = u_strdup(name);
    if (!q->name) { u_free(q); DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: strdup failed"); return NULL; }

    q->res = adns_open_resolver(servers, count, timeout_ms, attempts);
    if (!q->res) { u_free(q->name); u_free(q); return NULL; }

    if (dns_res_submit(q->res, name, DNS_T_A, DNS_C_IN) != 0) {
        DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: dns_res_submit failed for '%s'", name);
        dns_res_close(q->res);
        u_free(q->name);
        u_free(q);
        return NULL;
    }

    DEBUG_DEBUG(ADNS_DEBUG_CAT, "adns: resolving '%s' via %d server(s), timeout=%dms attempts=%d",
                name, count, timeout_ms, attempts);

    q->call_soon_id = uasync_call_soon(ua, q, adns_drive_trampoline);
    if (!q->call_soon_id) {
        DEBUG_ERROR(ADNS_DEBUG_CAT, "adns: uasync_call_soon failed for '%s'", name);
        dns_res_close(q->res);
        u_free(q->name);
        u_free(q);
        return NULL;
    }
    return q;
}

void adns_cancel(struct adns_query* q) {
    if (!q || q->finished) return;
    q->finished = 1;
    if (q->call_soon_id) { uasync_call_soon_cancel(q->ua, q->call_soon_id); q->call_soon_id = NULL; }
    if (q->fast) {
        u_free(q->name);
        u_free(q);
        return;
    }
    adns_teardown(q);
    u_free(q->name);
    u_free(q);
}
