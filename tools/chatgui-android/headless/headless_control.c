/*
 * headless_control.c — JSON control socket implementation
 */
#include "headless_control.h"
#include "../libutun_lite/utun_config_api.h"
#include "../../../src/chat/invite_link.h"
#include "../libutun_lite/instance_lite.h"
#include "../../../src/chat/chat_setting.h"
#include "../../../src/chat/chat_sync.h"
#include "../../../lib/debug_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef __linux__
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#endif

/* ── Simple JSON helpers (no library dependency) ── */

static int json_get_int(const char* s, const char* key, int def) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(s, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    return (int)strtol(p, NULL, 10);
}

static int json_get_str(const char* s, const char* key, char* out, size_t out_sz) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(s, pat);
    if (!p) return -1;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) { p++; out[i++] = *(p++); }
        else out[i++] = *(p++);
    }
    out[i] = '\0';
    return 0;
}

static const char* json_get_cmd(const char* s) {
    const char* p = strstr(s, "\"cmd\":\"");
    if (!p) return NULL;
    return p + 7;
}

static const char* json_get_cmd_end(const char* cmd_start) {
    const char* p = cmd_start;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) p++;
        p++;
    }
    return p;
}

/* ── Client state ── */

#define MAX_CLIENTS 16
#define BUF_SIZE    16384

struct control_client {
    int fd;
    char recv_buf[BUF_SIZE];
    int recv_len;
    int subscribed;
    struct control_client* next;
};

static struct control_client* g_clients = NULL;
static int g_listen_fd = -1;
static int g_running = 0;

/* ── Forward declarations ── */

static void send_response(int fd, int id, const char* ok_data, const char* error);
static void handle_command(struct control_client* cli, const char* json);
static void send_event_to_client(struct control_client* cli, const char* event_json);

/* ── Init / Destroy ── */

int headless_control_init(struct UASYNC* ua, const char* bind_ip, int port) {
    (void)ua;
    if (port <= 0 || port > 65535) return -1;
    if (!bind_ip) bind_ip = "127.0.0.1";

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { fprintf(stderr, "control: socket() failed: %s\n", strerror(errno)); return -1; }

    int reuse = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, bind_ip, &addr.sin_addr);

    if (bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "control: bind(%s:%d) failed: %s\n", bind_ip, port, strerror(errno));
        close(g_listen_fd); g_listen_fd = -1; return -1;
    }

    if (listen(g_listen_fd, 5) < 0) {
        fprintf(stderr, "control: listen() failed: %s\n", strerror(errno));
        close(g_listen_fd); g_listen_fd = -1; return -1;
    }

    int flags = fcntl(g_listen_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(g_listen_fd, F_SETFL, flags | O_NONBLOCK);

    g_running = 1;
    printf("control: listening on %s:%d\n", bind_ip, port);
    return 0;
}

void headless_control_destroy(void) {
    struct control_client* c = g_clients;
    while (c) { struct control_client* next = c->next; if (c->fd >= 0) close(c->fd); free(c); c = next; }
    g_clients = NULL;
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    g_running = 0;
}

/* ── Process (call from main loop) ── */

void headless_control_poll(void) {
    if (!g_running || g_listen_fd < 0) return;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(g_listen_fd, &rfds);
    int maxfd = g_listen_fd;

    for (struct control_client* c = g_clients; c; c = c->next) {
        FD_SET(c->fd, &rfds);
        if (c->fd > maxfd) maxfd = c->fd;
    }

    struct timeval tv = {0, 100000}; /* 100ms */
    int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (n <= 0) return;

    /* Accept new connections */
    if (FD_ISSET(g_listen_fd, &rfds)) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd >= 0) {
            int flags = fcntl(cfd, F_GETFL, 0);
            if (flags >= 0) fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
            struct control_client* cli = calloc(1, sizeof(*cli));
            if (cli) { cli->fd = cfd; cli->next = g_clients; g_clients = cli; }
            else close(cfd);
        }
    }

    /* Read from clients */
    for (struct control_client* c = g_clients; c; c = c->next) {
        if (!FD_ISSET(c->fd, &rfds)) continue;
        char tmp[4096];
        ssize_t r = read(c->fd, tmp, sizeof(tmp) - 1);
        if (r <= 0) { close(c->fd); c->fd = -1; continue; }
        for (ssize_t i = 0; i < r; i++) {
            if (tmp[i] == '\n') {
                if (c->recv_len > 0) {
                    c->recv_buf[c->recv_len] = '\0';
                    handle_command(c, c->recv_buf);
                    c->recv_len = 0;
                }
            } else if (c->recv_len < (int)(BUF_SIZE - 1)) {
                c->recv_buf[c->recv_len++] = tmp[i];
            }
        }
    }

    /* Clean up closed clients */
    struct control_client** prev = &g_clients;
    while (*prev) {
        if ((*prev)->fd < 0) {
            struct control_client* dead = *prev;
            *prev = dead->next;
            free(dead);
        } else prev = &(*prev)->next;
    }
}

/* ── Send helpers ── */

static void send_raw(int fd, const char* data, size_t len) {
    if (fd < 0 || !data || len == 0) return;
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = write(fd, data + sent, len - sent);
        if (r <= 0) break;
        sent += (size_t)r;
    }
}

static void send_response(int fd, int id, const char* ok_data, const char* error) {
    char buf[4096];
    int len;
    if (error)
        len = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":false,\"error\":\"%s\"}\n", id, error);
    else if (ok_data)
        len = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"data\":%s}\n", id, ok_data);
    else
        len = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true}\n", id);
    if (len > 0) send_raw(fd, buf, (size_t)len);
}

static void send_event_to_client(struct control_client* cli, const char* event_json) {
    if (!cli || !cli->subscribed || cli->fd < 0) return;
    send_raw(cli->fd, event_json, strlen(event_json));
    /* Append \n if not present */
    size_t elen = strlen(event_json);
    if (elen == 0 || event_json[elen - 1] != '\n')
        send_raw(cli->fd, "\n", 1);
}

/* ── Event broadcasting ── */

void headless_control_event(const char* json_event) {
    for (struct control_client* c = g_clients; c; c = c->next)
        send_event_to_client(c, json_event);
}

void headless_control_broadcast_log(const char* level, const char* category, const char* msg) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "{\"event\":\"log\",\"level\":\"%s\",\"cat\":\"%s\",\"msg\":\"%s\"}",
             level ? level : "info", category ? category : "", msg ? msg : "");
    headless_control_event(buf);
}

/* ── Command handlers ── */

static int handle_ping(int fd, int id, const char* json) {
    (void)json;
    send_response(fd, id, "{\"pong\":true,\"uptime\":0}", NULL);
    return 0;
}

static int handle_status(int fd, int id, const char* json) {
    (void)json;
    send_response(fd, id, "{\"node_id\":\"0\",\"uptime\":0,\"peers\":0,\"channels\":0}", NULL);
    return 0;
}

static int handle_connections(int fd, int id, const char* json) {
    (void)json;
    send_response(fd, id, "{\"connections\":[]}", NULL);
    return 0;
}

static int handle_subscribe(int fd, int id, const char* json) {
    int enable = json_get_int(json, "enable", 1);
    for (struct control_client* c = g_clients; c; c = c->next)
        if (c->fd == fd) { c->subscribed = enable; break; }
    send_response(fd, id, enable ? "{\"subscribed\":true}" : "{\"subscribed\":false}", NULL);
    return 0;
}

static int handle_debug_level(int fd, int id, const char* json) {
    (void)json;
    send_response(fd, id, "{\"global\":\"info\"}", NULL);
    return 0;
}

static int handle_quit(int fd, int id, const char* json) {
    (void)json;
    send_response(fd, id, "{\"bye\":true}", NULL);
    return -1;
}

static int handle_join(int fd, int id, const char* json) {
    char link[1024];
    if (json_get_str(json, "link", link, sizeof(link)) < 0) {
        send_response(fd, id, NULL, "missing link");
        return 0;
    }
    struct InviteData d;
    char err[256];
    if (invite_link_decode(link, strlen(link), &d, err, sizeof(err)) < 0) {
        send_response(fd, id, NULL, err);
        return 0;
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "join: peer_x=%s addrs=%u",
               sc_public_key_hex(d.pubkey, (char[65]){0}), d.addrCount);

    for (int i = 0; i < (int)d.addrCount; i++) {
        struct InviteAddr* a = &d.addrs[i];
        char ip[64]; snprintf(ip, sizeof(ip), a->family == 6 ? "v6" : "%d.%d.%d.%d",
            a->address[0], a->address[1], a->address[2], a->address[3]);
        const char* proto = a->proto == 1 ? "UDP" : a->proto == 2 ? "TCP" : a->proto == 3 ? "UDP+TCP" : "?";
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "  addr[%d]: %s:%d %s sock=%d", i, ip, (int)a->port, proto, (int)a->socketId);
    }

    uint8_t addrs_buf[512];
    int addrs_len = invite_serialize_addrs(&d, addrs_buf, sizeof(addrs_buf));
    if (addrs_len <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "join: invite_serialize_addrs failed");
        send_response(fd, id, NULL, "internal error: serialize failed");
        return 0;
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "join: calling chat_sync_join_link...");
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "join: no instance running — drop");
        send_response(fd, id, NULL, "instance not running");
        return 0;
    }
    chat_sync_join_link(inst, 1, link, strlen(link));

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"x25519\":\"%s\",\"addrs\":%d,\"connecting\":true}",
             sc_public_key_hex(d.pubkey, (char[65]){0}), (int)d.addrCount);
    send_response(fd, id, resp, NULL);
    return 0;
}

static int handle_chat_setting(int fd, int id, const char* json) {
    char name[128], value[128];
    if (json_get_str(json, "name", name, sizeof(name)) < 0 || json_get_str(json, "value", value, sizeof(value)) < 0) {
        send_response(fd, id, NULL, "missing name or value");
        return 0;
    }
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst) { send_response(fd, id, NULL, "instance not running"); return 0; }
    int rc = chat_setting_set(inst, name, value);
    if (rc == -1) { send_response(fd, id, NULL, "unknown setting"); return 0; }
    if (rc == -2) { send_response(fd, id, NULL, "invalid value"); return 0; }
    if (rc < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "headless setting saved but runtime apply failed name=%s rc=%d", name, rc);
        send_response(fd, id, NULL, "setting saved but runtime apply failed; retry setting"); return 0;
    }
    printf("control: chat_setting %s=%s\n", name, value);
    send_response(fd, id, "{\"set\":true}", NULL);
    return 0;
}

/* ── Command dispatch ── */

typedef int (*cmd_handler_fn)(int fd, int id, const char* json);

static const char* const g_cmd_names[] = {
    "ping", "status", "connect", "disconnect", "connections",
    "create_channel", "channels", "send", "messages",
    "debug_level", "subscribe", "join", "chat_setting", "quit", NULL
};

static cmd_handler_fn g_cmd_handlers[] = {
    handle_ping, handle_status, NULL, NULL, handle_connections,
    NULL, NULL, NULL, NULL,
    handle_debug_level, handle_subscribe, handle_join, handle_chat_setting, handle_quit
};

static void handle_command(struct control_client* cli, const char* json) {
    if (!cli || !json) return;

    int id = json_get_int(json, "id", 0);
    const char* cmd_start = json_get_cmd(json);
    if (!cmd_start) { send_response(cli->fd, id, NULL, "missing cmd"); return; }

    const char* cmd_end = json_get_cmd_end(cmd_start);
    size_t cmd_len = (size_t)(cmd_end - cmd_start);
    if (cmd_len == 0 || cmd_len > 31) { send_response(cli->fd, id, NULL, "invalid cmd"); return; }

    char cmd[32];
    memcpy(cmd, cmd_start, cmd_len);
    cmd[cmd_len] = '\0';

    for (int i = 0; g_cmd_names[i]; i++) {
        if (strcmp(cmd, g_cmd_names[i]) == 0) {
            if (g_cmd_handlers[i]) {
                int rc = g_cmd_handlers[i](cli->fd, id, json);
                if (rc < 0) { close(cli->fd); cli->fd = -1; }
            } else {
                send_response(cli->fd, id, NULL, "not implemented");
            }
            return;
        }
    }
    send_response(cli->fd, id, NULL, "unknown command");
}
