// reality_relay.c — живой релей неавторизованных клиентов на реальный HTTPS-сайт
//
// Двухсторонний TCP-пайп: клиент (приславший невалидный ClientHello) ↔ реальный
// сайт (dest). initial_data (уже прочитанный ClientHello) пересылается на dest
// первым. Поддержан half-close: FIN с одной стороны закрывает только свою
// сторону записи на другой, чтобы ответ сайта дошёл до клиента полностью.
#include "reality_relay.h"
#include "reality_io.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif
#include "../lib/async_dns.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

#define RELAY_IO_BUF 65536
#define RELAY_WARN_CONNS 64

#ifdef _WIN32
#define RELAY_SHUT_WR SD_SEND
#define RELAY_IN_PROGRESS WSAEINPROGRESS
#else
#define RELAY_SHUT_WR SHUT_WR
#define RELAY_IN_PROGRESS EINPROGRESS
#endif

struct reality_relay {
    struct reality_owner *owner;
    struct UASYNC *ua;
    socket_t client_sock;
    void    *client_sid;
    socket_t dest_sock;
    void    *dest_sid;
    int      connecting;    // 1 = ждём завершения connect() к dest

    void    *idle_timer;    // handle таймера простоя (NULL если выключен)
    int      idle_timeout_tb; // время простоя в 0.1ms (0 = таймер выключен)

    char     host[256];     // dest hostname (для async DNS и логирования)
    uint16_t port;          // dest port
    struct adns_query *dns_q;   // pending async DNS query (NULL если нет)

    uint8_t *c2d; size_t c2d_len, c2d_off;   // pending client→dest
    uint8_t *d2c; size_t d2c_len, d2c_off;   // pending dest→client

    int client_read_eof;    // клиент закрыл свою сторону записи
    int dest_read_eof;      // dest закрыл свою сторону записи
    int closed;
    void* free_call;       // отложенное освобождение, отменяемое при owner shutdown

    size_t bytes_c2d, bytes_d2c;

    struct reality_relay *next;   // цепочка активных релеев (для shutdown)
};


static void relay_free(struct reality_relay *r);
static int relay_touch(struct reality_relay *r);
static void relay_idle_timeout_cb(void *arg);
static void relay_flush_c2d(struct reality_relay *r);
static void relay_flush_d2c(struct reality_relay *r);
static void relay_client_read_cb(socket_t sock, void *arg);
static void relay_dest_read_cb(socket_t sock, void *arg);
static void relay_client_write_cb(socket_t sock, void *arg);
static void relay_dest_write_cb(socket_t sock, void *arg);
static void relay_except_cb(socket_t sock, void *arg);

static void relay_free_cb(void *arg) {
    struct reality_relay* r = arg;
    struct reality_relay** pp = &r->owner->closed_relays;
    while (*pp && *pp != r) pp = &(*pp)->next;
    if (*pp) *pp = r->next;
    u_free(r);
}

static void relay_free(struct reality_relay *r) {
    if (!r || r->closed) return;
    r->closed = 1;
    r->owner->relay_count--;
    {   // вынуть из реестра активных релеев
        struct reality_relay **pp = &r->owner->relays;
        while (*pp && *pp != r) pp = &(*pp)->next;
        if (*pp) *pp = r->next;
        r->next = NULL;
    }
    if (r->idle_timer) { uasync_cancel_timeout(r->ua, r->idle_timer); r->idle_timer = NULL; }
    if (r->dns_q) { adns_cancel(r->dns_q); r->dns_q = NULL; }
    if (r->client_sid) { uasync_remove_socket_t(r->ua, r->client_sock); r->client_sid = NULL; }
    if (r->dest_sid) { uasync_remove_socket_t(r->ua, r->dest_sock); r->dest_sid = NULL; }
    if (r->client_sock != SOCKET_INVALID) { socket_close_wrapper(r->client_sock); r->client_sock = SOCKET_INVALID; }
    if (r->dest_sock != SOCKET_INVALID) { socket_close_wrapper(r->dest_sock); r->dest_sock = SOCKET_INVALID; }
    if (r->c2d) { u_free(r->c2d); r->c2d = NULL; }
    if (r->d2c) { u_free(r->d2c); r->d2c = NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "reality_relay closed: c2d=%zu d2c=%zu", r->bytes_c2d, r->bytes_d2c);
    r->next = r->owner->closed_relays; r->owner->closed_relays = r;
    r->free_call = uasync_call_soon(r->ua, r, relay_free_cb);
    if (!r->free_call) DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay free scheduling failed; context retained until owner cleanup");
}

// Перезапускает таймер простоя после чтения или успешной отправки данных.
static int relay_touch(struct reality_relay *r) {
    if (r->idle_timeout_tb <= 0) return 0;
    void* timer = uasync_set_timeout(r->ua, r->idle_timeout_tb, r, relay_idle_timeout_cb, "relay_idle");
    if (!timer) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay idle timer allocation failed"); return -1; }
    if (r->idle_timer) { uasync_cancel_timeout(r->ua, r->idle_timer); r->idle_timer = NULL; }
    r->idle_timer = timer;
    return 0;
}

static void relay_idle_timeout_cb(void *arg) {
    struct reality_relay *r = (struct reality_relay *)arg;
    r->idle_timer = NULL;   // handle освобождён process_timeouts сразу после колбэка
    DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay: idle timeout %ds c2d=%zu d2c=%zu — closing",
               r->idle_timeout_tb / 10000, r->bytes_c2d, r->bytes_d2c);
    relay_free(r);
}

static void relay_flush_c2d(struct reality_relay *r) {
    if (r->closed || !r->c2d) return;
    while (r->c2d_off < r->c2d_len) {
        ssize_t sent = socket_send(r->dest_sock, r->c2d + r->c2d_off, r->c2d_len - r->c2d_off);
        if (sent < 0) {
            int err = socket_get_error();
            if (err == ERR_INTR) continue;
            if (err == ERR_AGAIN || err == ERR_WOULDBLOCK) {
                if (uasync_set_socket_write(r->ua, r->dest_sid, 1) != ERR_OK) {
                    DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot wait for dest write"); relay_free(r);
                }
                return;
            }
            DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay send to dest failed err=%d", err);
            relay_free(r); return;
        }
        if (sent == 0) { DEBUG_WARN(DEBUG_CATEGORY_REALITY, "relay send to dest made no progress"); relay_free(r); return; }
        r->c2d_off += (size_t)sent;
        r->bytes_c2d += (size_t)sent;
        if (relay_touch(r) < 0) { relay_free(r); return; }
    }
    u_free(r->c2d); r->c2d = NULL;
    if (uasync_set_socket_write(r->ua, r->dest_sid, 0) != ERR_OK ||
        (!r->client_read_eof && uasync_set_socket_read(r->ua, r->client_sid, 1) != ERR_OK)) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot resume client after dest flush"); relay_free(r);
    }
}

static void relay_flush_d2c(struct reality_relay *r) {
    if (r->closed || !r->d2c) return;
    while (r->d2c_off < r->d2c_len) {
        ssize_t sent = socket_send(r->client_sock, r->d2c + r->d2c_off, r->d2c_len - r->d2c_off);
        if (sent < 0) {
            int err = socket_get_error();
            if (err == ERR_INTR) continue;
            if (err == ERR_AGAIN || err == ERR_WOULDBLOCK) {
                if (uasync_set_socket_write(r->ua, r->client_sid, 1) != ERR_OK) {
                    DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot wait for client write"); relay_free(r);
                }
                return;
            }
            DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay send to client failed err=%d", err);
            relay_free(r); return;
        }
        if (sent == 0) { DEBUG_WARN(DEBUG_CATEGORY_REALITY, "relay send to client made no progress"); relay_free(r); return; }
        r->d2c_off += (size_t)sent;
        r->bytes_d2c += (size_t)sent;
        if (relay_touch(r) < 0) { relay_free(r); return; }
    }
    u_free(r->d2c); r->d2c = NULL;
    if (uasync_set_socket_write(r->ua, r->client_sid, 0) != ERR_OK ||
        (!r->dest_read_eof && uasync_set_socket_read(r->ua, r->dest_sid, 1) != ERR_OK)) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot resume dest after client flush"); relay_free(r);
    }
}

static void relay_client_read_cb(socket_t sock, void *arg) {
    struct reality_relay *r = (struct reality_relay *)arg;
    (void)sock;
    if (r->closed || r->c2d) return;
    uint8_t *buf = u_malloc(RELAY_IO_BUF);
    if (!buf) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay client receive allocation failed"); relay_free(r); return; }
    ssize_t n = recv(r->client_sock, buf, RELAY_IO_BUF, 0);
    if (n < 0) {
        int err = socket_get_error();
        if (err == ERR_AGAIN || err == ERR_WOULDBLOCK || err == ERR_INTR) { u_free(buf); return; }
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay recv from client failed err=%d", err);
        u_free(buf); relay_free(r); return;
    }
    if (n == 0) {
        u_free(buf);
        r->client_read_eof = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "relay client EOF: dest_eof=%d", r->dest_read_eof);
        if (uasync_set_socket_read(r->ua, r->client_sid, 0) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot stop client read at EOF"); relay_free(r); return;
        }
        if (shutdown(r->dest_sock, RELAY_SHUT_WR) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_REALITY, "relay dest half-close failed: err=%d", socket_get_error());
        if (r->dest_read_eof) relay_free(r);
        return;
    }
#ifdef UTUN_HAVE_STANDBY
    {
        struct sockaddr_storage sa; socklen_t slen = sizeof(sa);
        char who[64];
        if (getpeername(r->client_sock, (struct sockaddr*)&sa, &slen) == 0)
            snprintf(who, sizeof(who), "addr=%s", sockaddr_storage_to_str(&sa).str);
        else snprintf(who, sizeof(who), "?");
        standby_log_rx("reality relay", who);
    }
#endif
    r->c2d = buf; r->c2d_len = (size_t)n; r->c2d_off = 0;
    if (uasync_set_socket_read(r->ua, r->client_sid, 0) != ERR_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot pause client for backpressure"); relay_free(r); return;
    }
    if (relay_touch(r) < 0) { relay_free(r); return; }
    relay_flush_c2d(r);
}

static void relay_dest_read_cb(socket_t sock, void *arg) {
    struct reality_relay *r = (struct reality_relay *)arg;
    (void)sock;
    if (r->closed || r->d2c) return;
    uint8_t *buf = u_malloc(RELAY_IO_BUF);
    if (!buf) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay dest receive allocation failed"); relay_free(r); return; }
    ssize_t n = recv(r->dest_sock, buf, RELAY_IO_BUF, 0);
    if (n < 0) {
        int err = socket_get_error();
        if (err == ERR_AGAIN || err == ERR_WOULDBLOCK || err == ERR_INTR) { u_free(buf); return; }
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay recv from dest failed err=%d", err);
        u_free(buf); relay_free(r); return;
    }
    if (n == 0) {
        u_free(buf);
        r->dest_read_eof = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "relay dest EOF: client_eof=%d", r->client_read_eof);
        if (uasync_set_socket_read(r->ua, r->dest_sid, 0) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot stop dest read at EOF"); relay_free(r); return;
        }
        if (shutdown(r->client_sock, RELAY_SHUT_WR) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_REALITY, "relay client half-close failed: err=%d", socket_get_error());
        if (r->client_read_eof) relay_free(r);
        return;
    }
    r->d2c = buf; r->d2c_len = (size_t)n; r->d2c_off = 0;
    if (uasync_set_socket_read(r->ua, r->dest_sid, 0) != ERR_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot pause dest for backpressure"); relay_free(r); return;
    }
    if (relay_touch(r) < 0) { relay_free(r); return; }
    relay_flush_d2c(r);
}

static void relay_client_write_cb(socket_t sock, void *arg) {
    struct reality_relay *r = (struct reality_relay *)arg;
    (void)sock;
    if (r->closed) return;
    if (!r->d2c) {
        if (uasync_set_socket_write(r->ua, r->client_sid, 0) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot stop idle client write"); relay_free(r);
        }
        return;
    }
    relay_flush_d2c(r);
}

static void relay_dest_write_cb(socket_t sock, void *arg) {
    struct reality_relay *r = (struct reality_relay *)arg;
    if (r->closed) return;
    if (r->connecting) {
        int err = 0; socklen_t elen = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) < 0) err = socket_get_error();
        if (err != 0) {
            DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay connect to dest failed err=%d", err);
            relay_free(r); return;
        }
        r->connecting = 0;
        int opt = 1;
        if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt)) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_REALITY, "relay dest TCP_NODELAY failed: err=%d", socket_get_error());
        if (r->c2d) relay_flush_c2d(r);
        else if (uasync_set_socket_write(r->ua, r->dest_sid, 0) != ERR_OK ||
                 uasync_set_socket_read(r->ua, r->client_sid, 1) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot enable connected stream"); relay_free(r);
        }
        return;
    }
    if (!r->c2d) {
        if (uasync_set_socket_write(r->ua, r->dest_sid, 0) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot stop idle dest write"); relay_free(r);
        }
        return;
    }
    relay_flush_c2d(r);
}

/* HUP может прийти с последними данными: сначала дочитать поток, сохранив backpressure. */
static void relay_except_cb(socket_t sock, void *arg) {
    struct reality_relay* r = arg;
    if (r->closed) return;
    int err = 0; socklen_t size = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &size) < 0) err = socket_get_error();
    if (err) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "relay socket exception: side=%s err=%d",
            sock == r->client_sock ? "client" : "dest", err);
        relay_free(r); return;
    }
    if (sock == r->client_sock) {
        if (r->connecting) { DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "relay client closed before dest connection"); relay_free(r); return; }
        if (!r->client_read_eof) relay_client_read_cb(sock, r);
    } else if (!r->connecting && !r->dest_read_eof) relay_dest_read_cb(sock, r);
}

// Создать неблокирующий dest-сокет и начать connect() к уже разрезолвенному адресу.
// Вызывается сразу (literal IP) или из коллбэка async DNS.
static int relay_connect_dest(struct reality_relay *r, const struct sockaddr_in *addr) {
    socket_t dest_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (dest_sock == SOCKET_INVALID) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay: socket failed for %s:%u", r->host, r->port);
        relay_free(r);
        return -1;
    }
    r->dest_sock = dest_sock;
    if (socket_set_nonblocking(dest_sock) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay dest nonblocking setup failed: %d", socket_get_error()); relay_free(r); return -1;
    }

    int cr = connect(dest_sock, (const struct sockaddr *)addr, sizeof(*addr));
    if (cr < 0) {
        int err = socket_get_error();
        if (err != RELAY_IN_PROGRESS && err != ERR_WOULDBLOCK) {
            DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay connect %s:%u failed err=%d", r->host, r->port, err);
            relay_free(r);
            return -1;
        }
    } else {
        r->connecting = 0;
        int opt = 1;
        if (setsockopt(dest_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt)) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_REALITY, "relay dest TCP_NODELAY failed: err=%d", socket_get_error());
    }

    r->dest_sid = uasync_add_socket_t(r->ua, dest_sock, relay_dest_read_cb, relay_dest_write_cb, relay_except_cb, "reality_dest", r);
    if (!r->dest_sid) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay dest registration failed"); relay_free(r); return -1; }

    if (!r->connecting) {
        if (r->c2d) relay_flush_c2d(r);
        else if (uasync_set_socket_write(r->ua, r->dest_sid, 0) != ERR_OK ||
                 uasync_set_socket_read(r->ua, r->client_sid, 1) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot enable immediately connected stream"); relay_free(r);
        }
    }
    return r->closed ? -1 : 0;
}

static void relay_dns_done_cb(const struct adns_result *res, void *arg) {
    struct reality_relay *r = (struct reality_relay *)arg;
    r->dns_q = NULL;
    if (res->status == ADNS_OK && res->count > 0) {
        struct sockaddr_in sin = res->addrs[0];
        sin.sin_port = htons(r->port);
        const uint8_t *b = (const uint8_t *)&sin.sin_addr;
        DEBUG_INFO(DEBUG_CATEGORY_REALITY, "reality_relay: resolved %s -> %u.%u.%u.%u:%u",
                   r->host, b[0], b[1], b[2], b[3], r->port);
        relay_connect_dest(r, &sin);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay: DNS failed for %s status=%d", r->host, res->status);
        relay_free(r);
    }
}

int reality_relay_start(struct reality_owner *owner, struct UASYNC *ua, socket_t client_sock,
                        const char *dest,
                        const uint8_t *initial_data, size_t initial_len,
                        int idle_timeout_sec) {
    if (ua && client_sock != SOCKET_INVALID) uasync_remove_socket_t(ua, client_sock);
    if (!owner || !ua || client_sock == SOCKET_INVALID || !dest || !dest[0] || (!initial_data && initial_len) ||
        initial_len > UINT32_MAX || idle_timeout_sec < 0 || idle_timeout_sec > INT_MAX / 10000) {
        if (client_sock != SOCKET_INVALID) socket_close_wrapper(client_sock);
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_relay_start: invalid args");
        return -1;
    }

    struct reality_relay *r = u_calloc(1, sizeof(struct reality_relay));
    if (!r) { socket_close_wrapper(client_sock); DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_relay_start: calloc failed"); return -1; }
    r->owner = owner;
    r->ua = ua;
    r->client_sock = client_sock;
    r->dest_sock = SOCKET_INVALID;
    r->connecting = 1;
    r->idle_timeout_tb = idle_timeout_sec > 0 ? idle_timeout_sec * 10000 : 0;
    r->next = owner->relays; owner->relays = r;
    owner->relay_count++;
    if (owner->relay_count > RELAY_WARN_CONNS)
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay: %d concurrent relays (over %d) — возможен скан-флуд", owner->relay_count, RELAY_WARN_CONNS);
    if (relay_touch(r) < 0) { relay_free(r); return -1; } // покрывает DNS + connect

    // берём владение client_sock сразу (снимаем старую регистрацию) — до парсинга,
    // чтобы любая ошибка ниже не оставила сокет зарегистрированным в uasync.
    r->client_sid = uasync_add_socket_t(ua, client_sock, relay_client_read_cb, relay_client_write_cb, relay_except_cb, "reality_client", r);
    if (!r->client_sid) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay client registration failed"); relay_free(r); return -1; }
    // не читаем от клиента, пока dest не подключён (иначе буферили бы без границ во время DNS)
    if (uasync_set_socket_read(ua, r->client_sid, 0) != ERR_OK || uasync_set_socket_write(ua, r->client_sid, 0) != ERR_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay cannot suspend client during connect"); relay_free(r); return -1;
    }

    // парсим "host:port"
    {
        size_t dl = strlen(dest);
        if (dl >= sizeof(r->host)) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_relay_start: dest too long"); relay_free(r); return -1; }
        memcpy(r->host, dest, dl + 1);
        char *colon = strrchr(r->host, ':');
        if (colon) {
            *colon = '\0'; char* end;
            errno = 0;
            unsigned long port = strtoul(colon + 1, &end, 10);
            if (colon[1] < '0' || colon[1] > '9' || *end || errno || !port || port > 65535) {
                DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay invalid destination port: %s", dest); relay_free(r); return -1;
            }
            r->port = (uint16_t)port;
        }
        else r->port = 443;
        if (!r->host[0] || strchr(r->host, ':') || r->port == 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_relay_start: invalid/unsupported IPv6 dest '%s'", dest); relay_free(r); return -1;
        }
    }

    if (initial_len && initial_data) {
        r->c2d = u_malloc(initial_len);
        if (!r->c2d) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "relay initial buffer allocation failed: %zu", initial_len); relay_free(r); return -1; }
        memcpy(r->c2d, initial_data, initial_len);
        r->c2d_len = initial_len;
        r->c2d_off = 0;
    }

    // literal IPv4 — без DNS
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(r->port);
    if (inet_pton(AF_INET, r->host, &sin.sin_addr) == 1) {
        return relay_connect_dest(r, &sin);
    }

    // асинхронный A-запрос — не блокирует event loop
    r->dns_q = adns_resolve(ua, r->host, NULL, relay_dns_done_cb, r);
    if (!r->dns_q) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_relay: async DNS start failed for %s", r->host);
        relay_free(r);
        return -1;
    }

    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "reality_relay started: client→%s:%u initial=%zu (resolving)",
               r->host, r->port, initial_len);
    return 0;
}

void reality_relay_shutdown(struct reality_owner *owner) {
    if (!owner) return;
    int count = 0;
    while (owner->relays) {
        struct reality_relay *r = owner->relays;
        relay_free(r);   // сам вынимает r из g_relays и откладывает u_free через call_soon
        count++;
    }
    while (owner->closed_relays) {
        struct reality_relay* r = owner->closed_relays;
        owner->closed_relays = r->next;
        if (r->free_call) uasync_call_soon_cancel(r->ua, r->free_call);
        u_free(r);
    }
    if (count > 0)
        DEBUG_INFO(DEBUG_CATEGORY_REALITY, "reality_relay_shutdown: closed %d active relays", count);
}
