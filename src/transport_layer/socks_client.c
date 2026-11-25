// socks_client.c — SOCKS5 клиент: CONNECT (TCP) + UDP ASSOCIATE (UDP)
#include "socks_client.h"
#include "../lib/u_async.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SOCKS_CTRL_TIMEOUT_TB     100000   // 10 с в 0.1 мс (connect+greeting+auth+request)
#define SOCKS_CTRL_RBUF_SIZE      1024
#define SOCKS_UDP_RECONNECT_MIN_TB 10000   // 1 с в 0.1 мс
#define SOCKS_UDP_RECONNECT_MAX_TB 300000  // 30 с
#define SOCKS_UDP_MAX_PAYLOAD     2048
#ifdef _WIN32
#define SOCKS_IN_PROGRESS WSAEINPROGRESS
#else
#define SOCKS_IN_PROGRESS EINPROGRESS
#endif

// ── Общий control-канал: TCP к прокси + greeting + auth ───────────────
// Обе фазы (dial и udp) используют эту базу; после greeting+auth вызывается on_ready(c),
// где каждая сторона шлёт свой запрос (CONNECT / UDP ASSOCIATE) и подменяет process().
struct socks_ctrl {
    struct UASYNC *ua;
    struct socks_cfg cfg;
    socket_t sock;
    void *socket_id;
    void *timer;
    uint8_t have_auth;
    uint8_t done;           // dial: guard против повторного finish
    uint8_t rbuf[SOCKS_CTRL_RBUF_SIZE];
    size_t  rbuf_len;
    size_t  rbuf_need;      // читаем только текущую control-рамку, не забирая данные готового TCP-потока
    uint8_t sbuf[512];
    size_t  sbuf_len, sbuf_off;
    void (*on_ready)(struct socks_ctrl *c);          // greeting+auth OK → фаза 2
    void (*on_error)(struct socks_ctrl *c, int err); // любая фатальная ошибка
    void (*process)(struct socks_ctrl *c);           // текущий обработчик входящих
};

static void socks_ctrl_send(struct socks_ctrl *c, const uint8_t *data, size_t len);
static void socks_ctrl_flush(struct socks_ctrl *c);
static void socks_ctrl_process(struct socks_ctrl *c);
static void socks_ctrl_auth_process(struct socks_ctrl *c);
static void socks_ctrl_after_connect(struct socks_ctrl *c);
static void socks_ctrl_read_cb(socket_t sock, void *arg);
static void socks_ctrl_write_cb(socket_t sock, void *arg);
static void socks_ctrl_connect_wait_cb(socket_t sock, void *arg);
static void socks_ctrl_timeout_cb(void *arg);

static void socks_ctrl_flush(struct socks_ctrl *c) {
    while (c->sbuf_off < c->sbuf_len) {
        ssize_t sent = socket_send(c->sock, c->sbuf + c->sbuf_off, c->sbuf_len - c->sbuf_off);
        if (sent <= 0) {
            int err = sent < 0 ? socket_get_error() : EPIPE;
            if (err == ERR_INTR) continue;
            if (err == ERR_AGAIN || err == ERR_WOULDBLOCK) {
                if (uasync_set_socket_write(c->ua, c->socket_id, 1) == ERR_OK) return;
                DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: cannot enable write callback"); err = EIO;
            }
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: send failed err=%d", err);
            c->on_error(c, err);
            return;
        }
        c->sbuf_off += (size_t)sent;
    }
    c->sbuf_len = 0;
    c->sbuf_off = 0;
    if (uasync_set_socket_write(c->ua, c->socket_id, 0) != ERR_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: cannot disable write callback"); c->on_error(c, EIO);
    }
}

static void socks_ctrl_send(struct socks_ctrl *c, const uint8_t *data, size_t len) {
    if (len > sizeof(c->sbuf) || c->sbuf_off < c->sbuf_len) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: invalid control write len=%zu pending=%zu", len, c->sbuf_len - c->sbuf_off);
        c->on_error(c, ENOBUFS); return;
    }
    memcpy(c->sbuf, data, len);
    c->sbuf_len = len;
    c->sbuf_off = 0;
    socks_ctrl_flush(c);
}

static void socks_ctrl_read_cb(socket_t sock, void *arg) {
    struct socks_ctrl *c = (struct socks_ctrl *)arg;
    (void)sock;
    if (c->rbuf_need > sizeof(c->rbuf) || c->rbuf_need <= c->rbuf_len) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: invalid read state have=%zu need=%zu", c->rbuf_len, c->rbuf_need);
        c->on_error(c, ENOBUFS); return;
    }
    ssize_t n = recv(c->sock, c->rbuf + c->rbuf_len, c->rbuf_need - c->rbuf_len, 0);
    if (n < 0) {
        int err = socket_get_error();
        if (err == ERR_AGAIN || err == ERR_WOULDBLOCK || err == ERR_INTR) return;
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: recv failed err=%d", err);
        c->on_error(c, err);
        return;
    }
    if (n == 0) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks: control closed by proxy (EOF)"); c->on_error(c, ECONNRESET); return; }
    c->rbuf_len += (size_t)n;
    c->process(c);
}

static void socks_ctrl_write_cb(socket_t sock, void *arg) {
    struct socks_ctrl *c = (struct socks_ctrl *)arg;
    (void)sock;
    socks_ctrl_flush(c);
}

// Ответ на greeting: выбор метода (или auth).
static void socks_ctrl_process(struct socks_ctrl *c) {
    if (c->rbuf_len < 2) return;
    uint8_t ver = c->rbuf[0], method = c->rbuf[1];
    memmove(c->rbuf, c->rbuf + 2, c->rbuf_len - 2); c->rbuf_len -= 2;
    if (ver != 0x05) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: bad greeting ver=%d", ver); c->on_error(c, EPROTO); return; }
    if (method == 0x00) { c->on_ready(c); return; }
    if (method == 0x02 && c->have_auth) {
        size_t ulen = strlen(c->cfg.user), plen = strlen(c->cfg.pass);
        if (ulen > 255 || plen > 255) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: invalid credential lengths"); c->on_error(c, EINVAL); return; }
        uint8_t ab[2 + 255 + 1 + 255];
        size_t n = 0;
        ab[n++] = 0x01; ab[n++] = (uint8_t)ulen;
        memcpy(ab + n, c->cfg.user, ulen); n += ulen;
        ab[n++] = (uint8_t)plen;
        memcpy(ab + n, c->cfg.pass, plen); n += plen;
        c->process = socks_ctrl_auth_process;
        c->rbuf_need = 2;
        socks_ctrl_send(c, ab, n);
        return;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: no acceptable auth (offered=%d have_auth=%d)", method, c->have_auth);
    c->on_error(c, EACCES);
}

// Ответ на auth (RFC1929).
static void socks_ctrl_auth_process(struct socks_ctrl *c) {
    if (c->rbuf_len < 2) return;
    uint8_t ver = c->rbuf[0], status = c->rbuf[1];
    memmove(c->rbuf, c->rbuf + 2, c->rbuf_len - 2); c->rbuf_len -= 2;
    if (ver != 0x01 || status != 0x00) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: auth failed ver=%d status=%d", ver, status); c->on_error(c, EACCES); return; }
    c->on_ready(c);
}

static void socks_ctrl_after_connect(struct socks_ctrl *c) {
    uint8_t g[4];
    size_t n;
    g[0] = 0x05;
    if (c->have_auth) { g[1] = 0x02; g[2] = 0x00; g[3] = 0x02; n = 4; }
    else              { g[1] = 0x01; g[2] = 0x00;            n = 3; }
    c->process = socks_ctrl_process;
    c->rbuf_need = 2;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "socks: connected to proxy %s:%u, greeting (auth=%d)", c->cfg.host, c->cfg.port, c->have_auth);
    socks_ctrl_send(c, g, n);
}

static void socks_ctrl_connect_wait_cb(socket_t sock, void *arg) {
    struct socks_ctrl *c = (struct socks_ctrl *)arg;
    int err = 0;
    socklen_t el = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &el) < 0 || err != 0) {
        int e = err ? err : ECONNREFUSED;
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: connect to %s:%u failed err=%d", c->cfg.host, c->cfg.port, e);
        c->on_error(c, e);
        return;
    }
    uasync_remove_socket_t(c->ua, sock);
    c->socket_id = uasync_add_socket_t(c->ua, sock, socks_ctrl_read_cb, socks_ctrl_write_cb, NULL, "socks_ctrl", c);
    if (!c->socket_id) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: re-add socket failed"); c->on_error(c, ENOMEM); return; }
    socks_ctrl_after_connect(c);
}

static void socks_ctrl_timeout_cb(void *arg) {
    struct socks_ctrl *c = (struct socks_ctrl *)arg;
    c->timer = NULL;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks: connect/handshake timeout to %s:%u", c->cfg.host, c->cfg.port);
    c->on_error(c, ETIMEDOUT);
}

// Подключить control-канал к прокси (resolve + socket + connect + таймер).
// При немедленной ошибке возвращает -1 (сокет закрыт), cb не вызывается.
static int socks_ctrl_start(struct socks_ctrl *c, int timeout_tb) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", c->cfg.port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    int result = getaddrinfo(c->cfg.host, port_str, &hints, &res);
    if (result || !res) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: proxy resolve failed host=%s code=%d", c->cfg.host, result);
        if (res) freeaddrinfo(res);
        return -1;
    }
    struct sockaddr_storage pa = {0};
    socklen_t palen = (socklen_t)res->ai_addrlen;
    if ((res->ai_family != AF_INET && res->ai_family != AF_INET6) || res->ai_addrlen > sizeof(pa)) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: unsupported proxy address family=%d len=%zu", res->ai_family, (size_t)res->ai_addrlen);
        freeaddrinfo(res); return -1;
    }
    memcpy(&pa, res->ai_addr, res->ai_addrlen); freeaddrinfo(res);
    c->sock = socket(pa.ss_family, SOCK_STREAM, 0);
    if (c->sock == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: socket creation failed err=%d", socket_get_error()); return -1;
    }
    if (socket_set_nonblocking(c->sock) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: cannot enable nonblocking err=%d", socket_get_error()); goto fail;
    }
    result = connect(c->sock, (struct sockaddr*)&pa, palen);
    int error = result < 0 ? socket_get_error() : 0;
    if (result < 0 && error != SOCKS_IN_PROGRESS && error != ERR_WOULDBLOCK) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: connect to %s:%u failed err=%d", c->cfg.host, c->cfg.port, error); goto fail;
    }
    /* Даже готовый connect продолжится после возврата конструктора: send error может освободить dial. */
    c->socket_id = uasync_add_socket_t(c->ua, c->sock, NULL, socks_ctrl_connect_wait_cb, NULL, "socks_ctrl_connect", c);
    if (!c->socket_id) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: connect registration failed"); goto fail; }
    c->rbuf_need = 2;
    c->timer = uasync_set_timeout(c->ua, timeout_tb > 0 ? timeout_tb : SOCKS_CTRL_TIMEOUT_TB, c, socks_ctrl_timeout_cb, "socks_ctrl");
    if (!c->timer) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: handshake timer allocation failed"); goto fail; }
    return 0;
fail:
    if (c->socket_id) { uasync_remove_socket_t(c->ua, c->sock); c->socket_id = NULL; }
    socket_close_wrapper(c->sock); c->sock = SOCKET_INVALID;
    return -1;
}

// ── SOCKS5 CONNECT (dial) ─────────────────────────────────────────────

struct socks_dial {
    struct socks_ctrl ctrl;   // первый член — каст (struct socks_ctrl*)==&ctrl
    char target_host[256];
    uint16_t target_port;
    socks_dial_cb cb;
    void *arg;
};

static void dial_on_error(struct socks_ctrl *c, int err);
static void dial_reply_process(struct socks_ctrl *c);

static void dial_finish(struct socks_dial *d, int err) {
    struct socks_ctrl *c = &d->ctrl;
    if (c->done) return;
    c->done = 1;
    socket_t out = c->sock;
    if (c->timer) { uasync_cancel_timeout(c->ua, c->timer); c->timer = NULL; }
    if (c->socket_id) { uasync_remove_socket_t(c->ua, c->sock); c->socket_id = NULL; }
    socks_dial_cb cb = d->cb;
    void *arg = d->arg;
    if (err == 0) { c->sock = SOCKET_INVALID; }              // сокет переходит вызывающему
    else if (out != SOCKET_INVALID) { socket_close_wrapper(out); }
    u_free(d);
    cb(err == 0 ? out : SOCKET_INVALID, err, arg);
}

static void dial_on_ready(struct socks_ctrl *c) {
    struct socks_dial *d = (struct socks_dial *)c;
    uint8_t buf[4 + 1 + 255 + 2];
    size_t n = 0;
    buf[n++] = 0x05; buf[n++] = 0x01; buf[n++] = 0x00;   // CONNECT
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, d->target_host, &a4) == 1) {
        buf[n++] = 0x01; memcpy(buf + n, &a4, 4); n += 4;
    } else if (inet_pton(AF_INET6, d->target_host, &a6) == 1) {
        buf[n++] = 0x04; memcpy(buf + n, &a6, 16); n += 16;
    } else {
        size_t hl = strlen(d->target_host);
        if (hl > 255) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks: target hostname too long"); c->on_error(c, EINVAL); return; }
        buf[n++] = 0x03; buf[n++] = (uint8_t)hl; memcpy(buf + n, d->target_host, hl); n += hl;
    }
    uint16_t pb = htons(d->target_port);
    memcpy(buf + n, &pb, 2); n += 2;
    c->process = dial_reply_process;
    c->rbuf_need = 4;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "socks_dial: CONNECT %s:%u", d->target_host, d->target_port);
    socks_ctrl_send(c, buf, n);
}

static void dial_reply_process(struct socks_ctrl *c) {
    if (c->rbuf_len < 4) return;
    uint8_t ver = c->rbuf[0], rep = c->rbuf[1], atyp = c->rbuf[3];
    if (ver != 5 || c->rbuf[2] || rep) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_dial: invalid/rejected reply ver=%u rsv=%u rep=%u", ver, c->rbuf[2], rep);
        c->on_error(c, rep ? ECONNREFUSED : EPROTO); return;
    }
    size_t need = 4;
    if (atyp == 0x01) need += 4 + 2;
    else if (atyp == 0x04) need += 16 + 2;
    else if (atyp == 0x03) {
        if (c->rbuf_len < 5) { c->rbuf_need = 5; return; }
        if (!c->rbuf[4]) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_dial: empty BND domain"); c->on_error(c, EPROTO); return; }
        need += 1 + (size_t)c->rbuf[4] + 2;
    }
    else { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_dial: bad reply atyp=%d", atyp); c->on_error(c, EPROTO); return; }
    if (c->rbuf_len < need) { c->rbuf_need = need; return; }
    struct socks_dial *d = (struct socks_dial *)c;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_dial: CONNECT %s:%u established via %s:%u",
               d->target_host, d->target_port, c->cfg.host, c->cfg.port);
    dial_finish(d, 0);
}

static void dial_on_error(struct socks_ctrl *c, int err) {
    dial_finish((struct socks_dial *)c, err);
}

void *socks_dial(struct UASYNC *ua, const struct socks_cfg *cfg,
                 const char *target_host, uint16_t target_port,
                 socks_dial_cb cb, void *arg, int timeout_tb) {
    if (!ua || !cfg || !cfg->host[0] || !cfg->port || !target_host || !target_host[0] || !cb || !target_port ||
        !memchr(cfg->host, 0, sizeof(cfg->host)) || !memchr(cfg->user, 0, sizeof(cfg->user)) ||
        !memchr(cfg->pass, 0, sizeof(cfg->pass)) || strnlen(target_host, 256) > 255) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_dial: invalid arguments/config/target length"); return NULL;
    }
    struct socks_dial *d = u_calloc(1, sizeof(*d));
    if (!d) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_dial: alloc failed"); return NULL; }
    d->ctrl.ua = ua;
    d->ctrl.cfg = *cfg;
    d->ctrl.have_auth = cfg->user[0] != '\0';
    d->ctrl.sock = SOCKET_INVALID;
    d->ctrl.on_ready = dial_on_ready;
    d->ctrl.on_error = dial_on_error;
    d->ctrl.process = socks_ctrl_process;
    strncpy(d->target_host, target_host, sizeof(d->target_host) - 1);
    d->target_port = target_port;
    d->cb = cb;
    d->arg = arg;
    if (socks_ctrl_start(&d->ctrl, timeout_tb) < 0) { u_free(d); return NULL; }
    return d;
}

void socks_dial_cancel(void *h) {
    if (!h) return;
    struct socks_dial *d = (struct socks_dial *)h;
    struct socks_ctrl *c = &d->ctrl;
    if (c->done) return;
    c->done = 1;
    if (c->timer) { uasync_cancel_timeout(c->ua, c->timer); c->timer = NULL; }
    if (c->socket_id) { uasync_remove_socket_t(c->ua, c->sock); c->socket_id = NULL; }
    if (c->sock != SOCKET_INVALID) socket_close_wrapper(c->sock);
    u_free(d);
}

// ── SOCKS5 UDP ASSOCIATE ──────────────────────────────────────────────

struct socks_udp {
    struct socks_ctrl ctrl;   // первый член
    socket_t local_udp_fd;
    struct sockaddr_storage relay;
    uint8_t ready;
    uint8_t closing;
    void *reconnect_timer;
    uint32_t reconnect_delay_tb;
    socks_udp_ready_cb ready_cb;
    void *ready_arg;
};

static void udp_on_error(struct socks_ctrl *c, int err);
static void udp_reply_process(struct socks_ctrl *c);
static void udp_idle_process(struct socks_ctrl *c);
static void udp_reconnect_cb(void *arg);

static int udp_schedule_reconnect(struct socks_udp *s) {
    if (s->closing) return -1;
    s->ready = 0;
    if (s->ctrl.socket_id) { uasync_remove_socket_t(s->ctrl.ua, s->ctrl.sock); s->ctrl.socket_id = NULL; }
    if (s->ctrl.timer) { uasync_cancel_timeout(s->ctrl.ua, s->ctrl.timer); s->ctrl.timer = NULL; }
    if (s->ctrl.sock != SOCKET_INVALID) { socket_close_wrapper(s->ctrl.sock); s->ctrl.sock = SOCKET_INVALID; }
    s->ctrl.done = 0;
    s->ctrl.rbuf_len = 0;
    s->ctrl.sbuf_len = 0;
    s->ctrl.sbuf_off = 0;
    s->ctrl.process = socks_ctrl_process;
    s->ctrl.rbuf_need = 2;
    if (s->reconnect_delay_tb == 0) s->reconnect_delay_tb = SOCKS_UDP_RECONNECT_MIN_TB;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: reconnect in %u ms", s->reconnect_delay_tb / 10);
    s->reconnect_timer = uasync_set_timeout(s->ctrl.ua, (int)s->reconnect_delay_tb, s, udp_reconnect_cb, "socks_udp_rc");
    if (!s->reconnect_timer) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: reconnect timer allocation failed"); return -1; }
    s->reconnect_delay_tb *= 2;
    if (s->reconnect_delay_tb > SOCKS_UDP_RECONNECT_MAX_TB) s->reconnect_delay_tb = SOCKS_UDP_RECONNECT_MAX_TB;
    return 0;
}

static void udp_reconnect_cb(void *arg) {
    struct socks_udp *s = (struct socks_udp *)arg;
    s->reconnect_timer = NULL;
    if (s->closing) return;
    if (socks_ctrl_start(&s->ctrl, 0) < 0 && udp_schedule_reconnect(s) < 0 && s->ready_cb)
        s->ready_cb(s, ENOMEM, s->ready_arg);
}

static void udp_on_ready(struct socks_ctrl *c) {
    uint8_t buf[10];
    buf[0] = 0x05; buf[1] = 0x03; buf[2] = 0x00;   // UDP ASSOCIATE
    buf[3] = 0x01;                                   // ATYP=IPv4, DST=0.0.0.0:0
    memset(buf + 4, 0, 6);
    c->process = udp_reply_process;
    c->rbuf_need = 4;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "socks_udp: sending UDP ASSOCIATE");
    socks_ctrl_send(c, buf, 10);
}

static void udp_reply_process(struct socks_ctrl *c) {
    struct socks_udp *s = (struct socks_udp *)c;
    if (c->rbuf_len < 4) return;
    uint8_t ver = c->rbuf[0], rep = c->rbuf[1], atyp = c->rbuf[3];
    if (ver != 5 || c->rbuf[2] || rep) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: invalid/rejected reply ver=%u rsv=%u rep=%u", ver, c->rbuf[2], rep);
        c->on_error(c, rep ? ECONNREFUSED : EPROTO); return;
    }
    size_t need = 4;
    if (atyp == 0x01) need += 4 + 2;
    else if (atyp == 0x04) need += 16 + 2;
    else if (atyp == 0x03) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: domain relay unsupported"); c->on_error(c, EAFNOSUPPORT); return; }
    else { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: bad reply atyp=%d", atyp); c->on_error(c, EPROTO); return; }
    if (c->rbuf_len < need) { c->rbuf_need = need; return; }

    memset(&s->relay, 0, sizeof(s->relay));
    if (atyp == 0x01) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&s->relay;
        sa->sin_family = AF_INET;
        memcpy(&sa->sin_addr.s_addr, c->rbuf + 4, 4);
        memcpy(&sa->sin_port, c->rbuf + 8, 2);
    } else if (atyp == 0x04) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&s->relay;
        sa6->sin6_family = AF_INET6;
        memcpy(&sa6->sin6_addr, c->rbuf + 4, 16);
        memcpy(&sa6->sin6_port, c->rbuf + 20, 2);
    } else { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: unsupported relay family"); c->on_error(c, EAFNOSUPPORT); return; }

    uint16_t relay_port = ss_get_port(&s->relay);
    if (!relay_port) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: relay has zero port"); c->on_error(c, EPROTO); return; }
    int wildcard = atyp == 1 ? !((struct sockaddr_in*)&s->relay)->sin_addr.s_addr :
                              IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6*)&s->relay)->sin6_addr);
    if (wildcard) {
        struct sockaddr_storage peer = {0}; socklen_t size = sizeof(peer);
        if (getpeername(c->sock, (struct sockaddr*)&peer, &size) != 0 || peer.ss_family != s->relay.ss_family) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: cannot resolve wildcard relay from proxy peer");
            c->on_error(c, EAFNOSUPPORT); return;
        }
        s->relay = peer;
        if (atyp == 1) ((struct sockaddr_in*)&s->relay)->sin_port = htons(relay_port);
        else ((struct sockaddr_in6*)&s->relay)->sin6_port = htons(relay_port);
    }

    memmove(c->rbuf, c->rbuf + need, c->rbuf_len - need); c->rbuf_len -= need;
    s->ready = 1;
    s->reconnect_delay_tb = 0;
    if (c->timer) { uasync_cancel_timeout(c->ua, c->timer); c->timer = NULL; }
    c->process = udp_idle_process;
    c->rbuf_need = sizeof(c->rbuf);
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_udp: ASSOCIATE ready, relay=%s", sockaddr_storage_to_str(&s->relay).str);
    if (s->ready_cb) s->ready_cb(s, 0, s->ready_arg);
}

static void udp_idle_process(struct socks_ctrl *c) {
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: unexpected control data after READY bytes=%zu", c->rbuf_len);
    c->rbuf_len = 0;   // неожиданные данные на control-канале после READY — игнорируем
}

static void udp_on_error(struct socks_ctrl *c, int err) {
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: control error err=%d → reconnect", err);
    struct socks_udp* s = (struct socks_udp*)c;
    if (udp_schedule_reconnect(s) < 0 && s->ready_cb) s->ready_cb(s, ENOMEM, s->ready_arg);
}

struct socks_udp *socks_udp_associate(struct UASYNC *ua, const struct socks_cfg *cfg,
                                      socket_t local_udp_fd, socks_udp_ready_cb cb, void *arg) {
    if (!ua || !cfg || !cfg->host[0] || !cfg->port || local_udp_fd == SOCKET_INVALID ||
        !memchr(cfg->host, 0, sizeof(cfg->host)) || !memchr(cfg->user, 0, sizeof(cfg->user)) ||
        !memchr(cfg->pass, 0, sizeof(cfg->pass))) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: invalid arguments/config"); return NULL;
    }
    struct socks_udp *s = u_calloc(1, sizeof(*s));
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: alloc failed"); return NULL; }
    s->ctrl.ua = ua;
    s->ctrl.cfg = *cfg;
    s->ctrl.have_auth = cfg->user[0] != '\0';
    s->ctrl.sock = SOCKET_INVALID;
    s->ctrl.on_ready = udp_on_ready;
    s->ctrl.on_error = udp_on_error;
    s->ctrl.process = socks_ctrl_process;
    s->local_udp_fd = local_udp_fd;
    s->ready_cb = cb;
    s->ready_arg = arg;
    if (socks_ctrl_start(&s->ctrl, 0) < 0 && udp_schedule_reconnect(s) < 0) { socks_udp_destroy(s); return NULL; }
    return s;
}

void socks_udp_destroy(struct socks_udp *s) {
    if (!s) return;
    s->closing = 1;
    if (s->reconnect_timer) { uasync_cancel_timeout(s->ctrl.ua, s->reconnect_timer); s->reconnect_timer = NULL; }
    if (s->ctrl.timer) { uasync_cancel_timeout(s->ctrl.ua, s->ctrl.timer); s->ctrl.timer = NULL; }
    if (s->ctrl.socket_id) { uasync_remove_socket_t(s->ctrl.ua, s->ctrl.sock); s->ctrl.socket_id = NULL; }
    if (s->ctrl.sock != SOCKET_INVALID) socket_close_wrapper(s->ctrl.sock);
    u_free(s);
}

int socks_udp_is_ready(const struct socks_udp *s) { return s && s->ready; }
int socks_udp_get_relay(const struct socks_udp *s, struct sockaddr_storage *relay) {
    if (!s || !s->ready || !relay) return 0;
    *relay = s->relay; return 1;
}

ssize_t socks_udp_sendto(struct socks_udp *s, const uint8_t *data, size_t len,
                         const struct sockaddr_storage *dst) {
    if (!s || (!data && len) || !dst || (dst->ss_family != AF_INET && dst->ss_family != AF_INET6)) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: invalid send arguments"); return -1;
    }
    if (!s->ready) { DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "socks_udp: send deferred until relay ready"); return -1; }
    if (len > SOCKS_UDP_MAX_PAYLOAD) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: payload too large %zu", len); return -1; }
    uint8_t buf[SOCKS_UDP_MAX_PAYLOAD + 32];
    size_t off = 0;
    buf[off++] = 0; buf[off++] = 0;   // RSV
    buf[off++] = 0;                    // FRAG
    if (dst->ss_family == AF_INET) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)dst;
        buf[off++] = 0x01;
        memcpy(buf + off, &sa->sin_addr.s_addr, 4); off += 4;
        memcpy(buf + off, &sa->sin_port, 2); off += 2;
    } else {
        const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)dst;
        buf[off++] = 0x04;
        memcpy(buf + off, &sa6->sin6_addr, 16); off += 16;
        memcpy(buf + off, &sa6->sin6_port, 2); off += 2;
    }
    if (len) memcpy(buf + off, data, len);
    off += len;
    socklen_t rl = (s->relay.ss_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    ssize_t sent = socket_sendto(s->local_udp_fd, buf, off, (struct sockaddr *)&s->relay, rl);
    if (sent < 0) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: sendto relay failed err=%d", socket_get_error()); return -1; }
    if ((size_t)sent != off) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_udp: partial datagram sent=%zd expected=%zu", sent, off); return -1; }
    return (ssize_t)len;
}

ssize_t socks_udp_unwrap(const struct socks_udp *s, const uint8_t *raw, size_t raw_len,
                         uint8_t *payload, size_t cap, struct sockaddr_storage *src) {
    (void)s;
    if (!raw || raw_len < 10 || !src) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: missing/short input len=%zu", raw_len); return -1; }
    if (raw[0] != 0 || raw[1] != 0 || raw[2] != 0) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: bad RSV/FRAG"); return -1; }
    uint8_t atyp = raw[3];
    size_t hlen;
    if (atyp == 0x01) hlen = 4 + 4 + 2;
    else if (atyp == 0x04) hlen = 4 + 16 + 2;
    else if (atyp == 0x03) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: domain source unsupported"); return -1; }
    else { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: bad atyp=%d", atyp); return -1; }
    if (raw_len < hlen) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: short address len=%zu required=%zu", raw_len, hlen); return -1; }
    size_t plen = raw_len - hlen;
    if (plen > cap || (plen && !payload)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_udp: insufficient output capacity=%zu payload=%zu", cap, plen); return -1;
    }

    memset(src, 0, sizeof(*src));
    if (atyp == 0x01) {
        struct sockaddr_in *sa = (struct sockaddr_in *)src;
        sa->sin_family = AF_INET;
        memcpy(&sa->sin_addr.s_addr, raw + 4, 4);
        memcpy(&sa->sin_port, raw + 8, 2);
    } else if (atyp == 0x04) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)src;
        sa6->sin6_family = AF_INET6;
        memcpy(&sa6->sin6_addr, raw + 4, 16);
        memcpy(&sa6->sin6_port, raw + 20, 2);
    } else {
        return -1;   // domain — для ETCP-пиров не встречается
    }
    if (plen > 0) memmove(payload, raw + hlen, plen);
    return (ssize_t)plen;
}
