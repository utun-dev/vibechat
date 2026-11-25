// Реальные локальные TCP-сокеты и управляемый транспорт: проверяем границы
// handshake, подтверждение connect, изоляцию потоков и порядок DATA/FIN.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utun_instance.h"
#include "etcp_connections.h"
#include "etcp.h"
#include "proxy/socks_proxy.h"
#include "proxy/udp_proxy.h"
#include "proxy/icmp_proxy.h"
#include "lwip_tcp/lwip_tcp.h"
#include "lwip_tcp/lwip_tcp_priv.h"
#include "tun_if.h"
#include "debug_config.h"
#include "mem.h"
#include "test_clock.h"
#ifndef _WIN32
#include <unistd.h>

#else
#define SHUT_WR SD_SEND
#endif
static const uint8_t self_ed[32] = {
    0xe6, 0xa1, 0x78, 0x5a, 0x9f, 0xa5, 0x9b, 0xf5, 0x27, 0x8e, 0x4d, 0x14, 0xc7, 0x16, 0xdb, 0xe9,
    0x78, 0x23, 0x43, 0x09, 0x2f, 0x30, 0x1e, 0x02, 0xe8, 0xa6, 0x55, 0x49, 0x98, 0xf1, 0x3d, 0x64
};
static const uint8_t exit_ed[32] = {
    0xdd, 0x9d, 0x33, 0xdf, 0x43, 0xdf, 0xd0, 0x23, 0x81, 0x61, 0x2a, 0x90, 0x8f, 0xf3, 0xe1, 0x29,
    0x6e, 0x48, 0x62, 0xb7, 0x12, 0x1d, 0x59, 0x0a, 0x12, 0xed, 0xef, 0xba, 0xe7, 0xd7, 0x3d, 0x28
};


#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
struct message { struct UTUN_INSTANCE* inst; uint64_t group, peer; uint8_t svc, cmd; uint32_t sid; size_t len; uint8_t data[4096]; };
static struct message messages[512];
static unsigned message_count;
static struct UASYNC* ua;
static int short_http_deadline;
static int reject_retire;
static int immediate_response;
static int abort_exit_data;
static uint8_t data_reply;
static int fake_depth, reject_connect, short_open_slot, offline, fail_open_timer;
static void dispatch(struct UTUN_INSTANCE* inst, uint64_t peer, uint8_t svc, uint8_t cmd,
                     uint32_t sid, const void* data, size_t len);

// Допуск CHAT моделирует результат проверки членства обоих узлов.
static int chat_admitted;
const uint8_t* __wrap_group_policy_public_key(struct UTUN_INSTANCE* inst, const char* ch) {
    (void)inst; return !strcmp(ch, "dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28") ? exit_ed : NULL;
}
int __wrap_topo_group_chat_key_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    (void)inst;
    return chat_admitted && group == 111 && !memcmp(peer, (uint8_t[32]){11}, 32);
}

int __wrap_etcp_router_peer_reachable(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc) {
    (void)inst; (void)group; (void)svc; (void)ed; return !offline;
}

struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_lookup(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc) {
    static struct ll_queue q;
    static struct ETCP_ROUTER_CONN c;
    (void)group; (void)ed;
    if (!fake_depth || svc != ETCP_RT_ID_TCP_PROXY_SERVER) return NULL;
    q.count = fake_depth; c.send_q = &q; c.svc_id = svc; c.closed = 1; c.inst = inst;
    return &c;
}

void __wrap_etcp_router_conn_on_send_ready(struct ETCP_ROUTER_CONN* c, struct queue_waiter_handle* h, queue_threshold_callback_fn fn, void* arg) {
    (void)c; (void)h; (void)fn; (void)arg;
}

void* __real_uasync_set_timeout(struct UASYNC* ua, int time_tb, void* arg, void (*fn)(void*), const char* name);
void* __wrap_uasync_set_timeout(struct UASYNC* async, int time_tb, void* arg, void (*fn)(void*), const char* name) {
    if (fail_open_timer && !strcmp(name, "proxy_connected")) return NULL;
    if (short_http_deadline && (!strcmp(name, "http_deadline") || !strcmp(name, "proxy_connected"))) time_tb = 1000;
    if (short_open_slot && !strcmp(name, "proxy_open_slot")) time_tb = 1000;
    return __real_uasync_set_timeout(async, time_tb, arg, fn, name);
}

// Перехватывается только транспорт. Парсеры, tcp_io, очереди и lifecycle — настоящие.
int __wrap_etcp_route_send(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer,
                          struct ll_entry* entry, int force, int mode) {
    (void)mode;
    CHECK(message_count < 512 && entry->len >= 6 && entry->len <= 4102);
    struct message* m = &messages[message_count++];
    m->inst = inst; m->group = group; m->peer = peer; m->svc = entry->dgram[0]; m->cmd = entry->dgram[1];
    memcpy(&m->sid, entry->dgram + 2, 4); m->len = entry->len - 6;
    memcpy(m->data, entry->dgram + 6, m->len);
    queue_dgram_free(entry); queue_entry_free(entry);
    if (m->svc == ETCP_RT_ID_TCP_PROXY_SERVER && (m->cmd == TCP_PROXY_SUBCMD_CONNECT || m->cmd == TCP_PROXY_SUBCMD_DATA)) CHECK(!force);
    if (reject_connect && m->cmd == TCP_PROXY_SUBCMD_CONNECT) return -1;
    if (data_reply && m->svc == ETCP_RT_ID_TCP_PROXY_SERVER && m->cmd == TCP_PROXY_SUBCMD_DATA) {
        uint32_t credit = (uint32_t)m->len;
        uint8_t cmd = data_reply;
        data_reply = 0;
        dispatch(inst, peer, ETCP_RT_ID_TCP_PROXY_CLIENT, cmd, m->sid, &credit, cmd == TCP_PROXY_SUBCMD_WINDOW ? sizeof(credit) : 0);
    }
    if (abort_exit_data && m->svc == ETCP_RT_ID_TCP_PROXY_CLIENT && m->cmd == TCP_PROXY_SUBCMD_DATA) {
        abort_exit_data = 0;
        dispatch(inst, m->peer, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_ERROR, m->sid, NULL, 0);
    }
    if (reject_retire && (m->cmd == TCP_PROXY_SUBCMD_CLOSE || m->cmd == TCP_PROXY_SUBCMD_ERROR)) return -1;
    if (immediate_response && m->cmd == TCP_PROXY_SUBCMD_DATA) {
        int response = immediate_response; immediate_response = 0;
        const char* text = response == 1 ? "HTTP/1.1 413 Too Large\r\nContent-Length: 0\r\n\r\n" :
                           response == 3 ? "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\nRAW" :
                           response == 4 ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n" : "BROKEN\r\n\r\n";
        struct tcp_proxy_client* p = inst->tcp_proxy_client;
        CHECK(socks_proxy_handle_etcp(&p->http_conns, &p->http_conn_count, m->sid,
                                      TCP_PROXY_SUBCMD_DATA, (const uint8_t*)text, strlen(text)) == 1);
    }
    return 0;
}
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], struct ll_entry* e, int force, int mode) {
    uint64_t id = !memcmp(ed, self_ed, 32) ? inst->node_id : !memcmp(ed, exit_ed, 32) ? 42 : ed[0];
    CHECK(id);
    return __wrap_etcp_route_send(inst, group, id, e, force, mode);
}


// Сохраняем 12мс на обработку событий и проверку отсутствия преждевременных ответов.
static void pump(void) { test_clock_run_for(ua, 120); }

static socket_t listen_local(uint16_t* port) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd != SOCKET_INVALID);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(fd, (struct sockaddr*)&a, sizeof(a)) == 0 && listen(fd, 8) == 0);
    socklen_t len = sizeof(a); CHECK(getsockname(fd, (struct sockaddr*)&a, &len) == 0);
    *port = ntohs(a.sin_port); socket_set_nonblocking(fd);
    return fd;
}

static socket_t connect_local_buf(uint16_t port, int recv_bytes) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (recv_bytes) CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&recv_bytes, sizeof(recv_bytes)) == 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0);
    socket_set_nonblocking(fd); pump(); return fd;
}

static socket_t connect_local(uint16_t port) { return connect_local_buf(port, 0); }

static void send_bytes(socket_t fd, const void* bytes, size_t len) {
    CHECK(send(fd, bytes, len, 0) == (ssize_t)len); pump();
}

static void bounded_open_tests(struct UTUN_INSTANCE* inst) {
    const char request[] = "CONNECT 127.0.0.1:443 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    char reply[512];
    struct tcp_proxy_client* p = tcp_proxy_client_create(inst, ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed,
                                                         0, NULL, 1, "127.0.0.1:0");
    CHECK(p); inst->tcp_proxy_client = p;
    uint16_t port = socks_proxy_get_port(p->http_listen);
    socket_t fds[TCP_PROXY_CLIENT_OPEN_LIMIT + 1];
    fake_depth = ROUTER_MAX_SEND_Q_PACKETS; message_count = 0;
    for (int i = 0; i <= TCP_PROXY_CLIENT_OPEN_LIMIT; i++) {
        fds[i] = connect_local(port); send_bytes(fds[i], request, sizeof(request) - 1);
    }
    CHECK(p->opening == TCP_PROXY_CLIENT_OPEN_LIMIT && message_count == 0);
    CHECK(recv(fds[TCP_PROXY_CLIENT_OPEN_LIMIT], reply, sizeof(reply), 0) > 12 && !memcmp(reply, "HTTP/1.1 503", 12));
    socket_close_wrapper(fds[TCP_PROXY_CLIENT_OPEN_LIMIT]);
    struct linger reset = {.l_onoff = 1, .l_linger = 0};
    CHECK(setsockopt(fds[0], SOL_SOCKET, SO_LINGER, (const char*)&reset, sizeof(reset)) == 0);
    socket_close_wrapper(fds[0]); pump();
    CHECK(p->opening == TCP_PROXY_CLIENT_OPEN_LIMIT - 1 && !message_count);
    fds[0] = connect_local(port); send_bytes(fds[0], request, sizeof(request) - 1);
    CHECK(p->opening == TCP_PROXY_CLIENT_OPEN_LIMIT);
    fake_depth = 0; test_clock_run_for(ua, 1000);
    CHECK(message_count == TCP_PROXY_CLIENT_OPEN_LIMIT);
    for (unsigned i = 0; i < TCP_PROXY_CLIENT_OPEN_LIMIT; i++) {
        CHECK(messages[i].cmd == TCP_PROXY_SUBCMD_CONNECT);
        CHECK(socks_proxy_handle_etcp(&p->http_conns, &p->http_conn_count, messages[i].sid, TCP_PROXY_SUBCMD_ERROR, NULL, 0));
    }
    pump(); CHECK(!p->opening && !p->http_conn_count && !p->terminal_count);
    CHECK(message_count == TCP_PROXY_CLIENT_OPEN_LIMIT); // ERROR exit не отражается обратно
    for (int i = 0; i < TCP_PROXY_CLIENT_OPEN_LIMIT; i++) {
        CHECK(recv(fds[i], reply, sizeof(reply), 0) > 12 && !memcmp(reply, "HTTP/1.1 502", 12));
        socket_close_wrapper(fds[i]);
    }

    short_open_slot = 1; fake_depth = ROUTER_MAX_SEND_Q_PACKETS; message_count = 0;
    socket_t fd = connect_local(port); send_bytes(fd, request, sizeof(request) - 1);
    test_clock_run_for(ua, 1500); short_open_slot = 0;
    CHECK(recv(fd, reply, sizeof(reply), 0) > 12 && !memcmp(reply, "HTTP/1.1 504", 12));
    CHECK(!p->opening && !p->http_conn_count && !message_count);
    socket_close_wrapper(fd);

    fake_depth = 0; reject_connect = 1;
    fd = connect_local(port); send_bytes(fd, request, sizeof(request) - 1); reject_connect = 0;
    CHECK(recv(fd, reply, sizeof(reply), 0) > 12 && !memcmp(reply, "HTTP/1.1 502", 12));
    CHECK(!p->opening && !p->http_conn_count && message_count == 1 && messages[0].cmd == TCP_PROXY_SUBCMD_CONNECT);
    socket_close_wrapper(fd);

    offline = 1; fake_depth = ROUTER_MAX_SEND_Q_PACKETS; message_count = 0;
    fd = connect_local(port); send_bytes(fd, request, sizeof(request) - 1); offline = 0;
    CHECK(recv(fd, reply, sizeof(reply), 0) > 12 && !memcmp(reply, "HTTP/1.1 502", 12));
    CHECK(!p->opening && !p->http_conn_count && !message_count);
    socket_close_wrapper(fd);

    fake_depth = 0; fail_open_timer = 1;
    fd = connect_local(port); send_bytes(fd, request, sizeof(request) - 1); fail_open_timer = 0;
    CHECK(recv(fd, reply, sizeof(reply), 0) > 12 && !memcmp(reply, "HTTP/1.1 502", 12));
    CHECK(!p->opening && !p->http_conn_count && !message_count);
    socket_close_wrapper(fd);

    fake_depth = ROUTER_MAX_SEND_Q_PACKETS + TCP_PROXY_CLIENT_CONTROL_RESERVE; message_count = 0;
    tcp_proxy_client_retire(p, 1234, TCP_PROXY_SUBCMD_ERROR); pump();
    CHECK(p->terminal_count == 1 && !message_count);
    fake_depth--; test_clock_run_for(ua, 1000);
    CHECK(!p->terminal_count && message_count == 1 && messages[0].sid == 1234 && messages[0].cmd == TCP_PROXY_SUBCMD_ERROR);
    fake_depth = 0;
    tcp_proxy_client_destroy(p); inst->tcp_proxy_client = NULL; pump();
    puts("[PASS] 64 pending CONNECT, 65th refused, replacement, slot deadline, exit errors and control reserve");
}

static unsigned count_cmd(uint8_t cmd) {
    unsigned count = 0;
    for (unsigned i = 0; i < message_count; i++) if (messages[i].cmd == cmd) count++;
    return count;
}

static void dispatch_group(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, uint8_t svc, uint8_t cmd,
                     uint32_t sid, const void* data, size_t len) {
    struct ll_entry* e = queue_entry_new(0);
    CHECK(e);
    e->len = cmd ? TCP_PROXY_RECV_HDR_SIZE + len : ROUTER_SVC_HDR_SIZE;
    e->dgram = u_calloc(1, e->len); CHECK(e->dgram);
    e->dgram[0] = svc;
    /* Числовое поле оставляем одинаковым для всех пиров, потоки различаются по Ed25519. */
    uint8_t ed[32] = {(uint8_t)peer};
    memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, peer == 42 ? exit_ed : peer == inst->node_id ? self_ed : ed, 32);
    memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &group, 8);
    if (cmd) {
        e->dgram[ROUTER_SVC_PAYLOAD_OFF] = cmd;
        memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, &sid, 4);
        if (len) memcpy(e->dgram + TCP_PROXY_RECV_HDR_SIZE, data, len);
    }
    struct ETCP_CONN conn = {0}; conn.instance = inst;
    if (svc == ETCP_RT_ID_TCP_PROXY_SERVER) tcp_proxy_server_recv_cb(&conn, e);
    else tcp_proxy_client_router_recv_cb(&conn, e);
}

static void dispatch(struct UTUN_INSTANCE* inst, uint64_t peer, uint8_t svc, uint8_t cmd,
                     uint32_t sid, const void* data, size_t len) {
    dispatch_group(inst, TOPO_GROUP_UTUN, peer, svc, cmd, sid, data, len);
}

static void deliver(struct UTUN_INSTANCE* inst, uint64_t peer, uint8_t svc, uint8_t cmd,
                    uint32_t sid, const void* data, size_t len) {
    dispatch(inst, peer, svc, cmd, sid, data, len);
    pump();
}

// Открываемый поток должен сохранить оба направления и полный кредит до CONNECTED.
static void check_before_connected(struct UTUN_INSTANCE* inst, struct proxy_flow* flow) {
    CHECK(flow->opening && !flow->ready);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_DATA, flow->sid, "x", 1);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_FIN, flow->sid, NULL, 0);
    CHECK(flow->opening && !flow->ready && !flow->fin_received && flow->rx_credit == TCP_PROXY_WINDOW);
}

static void parser_tests(struct UTUN_INSTANCE* inst) {
    struct tcp_proxy_client* p = tcp_proxy_client_create(inst, ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed,
                                                        1, "127.0.0.1:0", 1, "127.0.0.1:0");
    CHECK(p); inst->tcp_proxy_client = p;
    uint16_t sp = socks_proxy_get_port(p->socks_listen), hp = socks_proxy_get_port(p->http_listen); CHECK(sp && hp);
    socket_t fd = connect_local(sp);
    // Greeting, CONNECT и ранние данные намеренно одним write.
    const uint8_t request[] = {5,1,0, 5,1,0,1,127,0,0,1,0,80, 'a','b','c'};
    message_count = 0; send_bytes(fd, request, sizeof(request));
    CHECK(count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 1 && count_cmd(TCP_PROXY_SUBCMD_DATA) == 0);
    uint8_t reply[256]; CHECK(recv(fd, reply, sizeof(reply), 0) == 2 && reply[1] == 0);
    uint32_t sid = p->socks_conns->stream_id;
    check_before_connected(inst, &p->socks_conns->flow);
    deliver(inst, 99, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, sid, NULL, 0);
    deliver(inst, 99, ETCP_RT_ID_TCP_PROXY_CLIENT, 0, 0, NULL, 0);
    CHECK(p->socks_conn_count == 1 && !p->socks_conns->flow.ready);
    data_reply = TCP_PROXY_SUBCMD_WINDOW;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, sid, NULL, 0);
    CHECK(recv(fd, reply, sizeof(reply), 0) == 10 && reply[1] == 0);
    CHECK(count_cmd(TCP_PROXY_SUBCMD_DATA) == 1 && messages[message_count-1].len == 3);
    CHECK(memcmp(messages[message_count-1].data, "abc", 3) == 0);
    CHECK(!data_reply && p->socks_conns->flow.tx_credit == TCP_PROXY_WINDOW && !p->socks_conns->tx_buf);
    puts("[PASS] SOCKS buffered DATA sent once with inline WINDOW and full credit restored");
    // Закрытое окно: FIN обоих направлений не должен уничтожать ожидающую передачу.
    p->socks_conns->flow.tx_credit = 0;
    message_count = 0; send_bytes(fd, "tail", 4);
    CHECK(shutdown(fd, SHUT_WR) == 0); pump();
    CHECK(count_cmd(TCP_PROXY_SUBCMD_FIN) == 0);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_FIN, sid, NULL, 0);
    CHECK(p->socks_conn_count == 1);
    uint32_t credit = 4;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_WINDOW, sid, &credit, 4);
    CHECK(count_cmd(TCP_PROXY_SUBCMD_DATA) == 1 && count_cmd(TCP_PROXY_SUBCMD_FIN) == 1);
    CHECK(messages[0].cmd == TCP_PROXY_SUBCMD_DATA && memcmp(messages[0].data, "tail", 4) == 0);
    CHECK(p->socks_conn_count == 0); socket_close_wrapper(fd);

    fd = connect_local(hp); message_count = 0;
    const char first[] = "CONNECT 127.0.0.1:443 HTTP/1.1\r\n";
    send_bytes(fd, first, sizeof(first)-1); CHECK(message_count == 0);
    CHECK(recv(fd, reply, sizeof(reply), 0) < 0);
    const char second[] = "Host: 127.0.0.1\r\n\r\nTLS";
    send_bytes(fd, second, sizeof(second)-1); CHECK(count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 1);
    sid = p->http_conns->stream_id;
    CHECK(recv(fd, reply, sizeof(reply), 0) < 0);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, sid, NULL, 0);
    ssize_t n = recv(fd, reply, sizeof(reply), 0); CHECK(n > 12 && memcmp(reply, "HTTP/1.1 200", 12) == 0);
    CHECK(messages[message_count-1].len == 3 && memcmp(messages[message_count-1].data, "TLS", 3) == 0);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_ERROR, sid, NULL, 0);
    socket_close_wrapper(fd);

    fd = connect_local(hp); message_count = 0;
    send_bytes(fd, first, sizeof(first)-1); send_bytes(fd, second, sizeof(second)-1);
    sid = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_ERROR, sid, NULL, 0);
    n = recv(fd, reply, sizeof(reply), 0); CHECK(n > 12 && memcmp(reply, "HTTP/1.1 502", 12) == 0);
    socket_close_wrapper(fd);

    fd = connect_local(sp); message_count = 0;
    const uint8_t auth[] = {5,1,2}; send_bytes(fd, auth, sizeof(auth));
    CHECK(recv(fd, reply, sizeof(reply), 0) == 2 && reply[1] == 255 && message_count == 0);
    socket_close_wrapper(fd);
    fd = connect_local(sp); message_count = 0;
    uint8_t v6[25] = {5,1,0,5,1,0,4}; v6[24] = 80;
    send_bytes(fd, v6, sizeof(v6));
    n = recv(fd, reply, sizeof(reply), 0); CHECK(n == 12 && reply[3] == 8 && message_count == 0);
    socket_close_wrapper(fd);
    // Большие заголовки и body, поступающий до CONNECTED, должны сохраниться и разбиться на DATA.
    fd = connect_local(hp); message_count = 0;
    char upload[32000], expected[32000];
    int header = snprintf(upload, sizeof(upload), "POST http://127.0.0.1/upload HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: ");
    memset(upload + header, 'x', 9000); header += 9000;
    header += snprintf(upload + header, sizeof(upload) - header, "\r\nContent-Length: 20000\r\n\r\n");
    memset(upload + header, 'B', 20000);
    for (int off = 0; off < header + 20000;) {
        int chunk = header + 20000 - off; if (chunk > 1024) chunk = 1024;
        send_bytes(fd, upload + off, chunk); off += chunk;
    }
    CHECK(count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 1 && count_cmd(TCP_PROXY_SUBCMD_DATA) == 0);
    sid = p->http_conns->stream_id;
    CHECK(p->http_conns->tc->read_queue->count <= 8);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, sid, NULL, 0);
    for (int i = 0; i < 10; i++) pump();
    size_t total = 0;
    for (unsigned i = 0; i < message_count; i++) if (messages[i].cmd == TCP_PROXY_SUBCMD_DATA) {
        CHECK(messages[i].len <= TCP_PROXY_CHUNK && total + messages[i].len <= sizeof(expected));
        memcpy(expected + total, messages[i].data, messages[i].len); total += messages[i].len;
    }
    CHECK(total < sizeof(expected)); expected[total] = 0;
    CHECK(strstr(expected, "POST /upload HTTP/1.1\r\nHost: 127.0.0.1:80\r\n"));
    CHECK(strstr(expected, "Content-Length: 20000\r\nConnection: close\r\n"));
    char* body = strstr(expected, "\r\n\r\n"); CHECK(body && total - (body + 4 - expected) == 20000);
    CHECK(memcmp(body + 4, upload + header, 20000) == 0 && count_cmd(TCP_PROXY_SUBCMD_FIN) == 1);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_ERROR, sid, NULL, 0);
    socket_close_wrapper(fd);
    tcp_proxy_client_destroy(p); inst->tcp_proxy_client = NULL; pump();
    puts("[PASS] SOCKS/HTTP split/coalesced headers, CONNECTED/error, source identity and half-close");
}

static size_t data_sent(uint32_t sid, char* out, size_t cap) {
    size_t len = 0;
    for (unsigned i = 0; i < message_count; i++) if (messages[i].sid == sid && messages[i].cmd == TCP_PROXY_SUBCMD_DATA) {
        CHECK(len + messages[i].len < cap);
        memcpy(out+len, messages[i].data, messages[i].len); len += messages[i].len;
    }
    out[len] = 0; return len;
}

static size_t client_reply(socket_t fd, char* out, size_t cap) {
    size_t len = 0;
    for (;;) {
        ssize_t n = recv(fd, out+len, cap-len-1, 0);
        if (n <= 0) break;
        len += n; CHECK(len+1 < cap);
    }
    out[len] = 0; return len;
}

static void upstream(struct UTUN_INSTANCE* inst, uint32_t sid, const char* text) {
    for (size_t off = 0; off < strlen(text);) {
        size_t n = strlen(text)-off; if (n > TCP_PROXY_CHUNK) n = TCP_PROXY_CHUNK;
        deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_DATA, sid, text+off, n); off += n;
    }
}

// Малое окно чтения и малый SO_SNDBUF оставляют часть DATA в tcp_io даже после успешного send.
static void tunnel_drain_tests(struct UTUN_INSTANCE* inst) {
    const uint8_t socks[] = {5,1,0, 5,1,0,1,127,0,0,1,0,80};
    const char connect[] = "CONNECT 127.0.0.1:443 HTTP/1.1\r\nHost: x\r\n\r\n";
    const char upgrade[] = "GET http://127.0.0.1/ HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
    uint8_t block[TCP_PROXY_CHUNK];
    char reply[TCP_PROXY_CHUNK];
    struct tcp_proxy_client* p = tcp_proxy_client_create(inst, ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed,
                                                         1, "127.0.0.1:0", 1, "127.0.0.1:0");
    CHECK(p); inst->tcp_proxy_client = p;
    for (int mode = 0; mode < 3; mode++) {
        message_count = 0;
        int buffer_bytes = 4096;
        socket_t fd = connect_local_buf(socks_proxy_get_port(mode ? p->http_listen : p->socks_listen), buffer_bytes);
        if (!mode) send_bytes(fd, socks, sizeof(socks));
        else send_bytes(fd, mode == 1 ? connect : upgrade, mode == 1 ? sizeof(connect)-1 : sizeof(upgrade)-1);
        struct socks_proxy_conn* c = mode ? p->http_conns : p->socks_conns;
        uint32_t sid = c->stream_id;
        deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, sid, NULL, 0);
        if (mode == 2) upstream(inst, sid, "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
        CHECK(c->state == SOCKS_STATE_RELAY && client_reply(fd, reply, sizeof(reply)) > 0);
        CHECK(setsockopt(c->tc->sock, SOL_SOCKET, SO_SNDBUF, (const char*)&buffer_bytes, sizeof(buffer_bytes)) == 0);
        if (mode == 1) {
            CHECK(shutdown(fd, SHUT_WR) == 0); pump();
            CHECK(c->flow.fin_sent && !uasync_timer_armed(c->drain_timer));
            test_clock_run_for(ua, 105000); CHECK(p->http_conn_count == 1);
        }
        for (unsigned offset = 0; offset < TCP_PROXY_WINDOW; offset += sizeof(block)) {
            for (size_t i = 0; i < sizeof(block); i++) block[i] = (uint8_t)((offset+i) % 251);
            dispatch(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_DATA, sid, block, sizeof(block));
        }
        pump(); CHECK(c->tc->write_buf || c->tc->write_queue->head);
        uint8_t cmd = mode == 1 ? TCP_PROXY_SUBCMD_FIN : TCP_PROXY_SUBCMD_CLOSE;
        dispatch(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, cmd, sid, NULL, 0);
        CHECK(uasync_timer_armed(c->drain_timer));
        if (mode == 2) {
            // Читатель возобновился. Все принятые байты должны прийти ровно один раз до EOF.
            size_t received = 0;
            for (int i = 0; i < 600; i++) {
                ssize_t n = recv(fd, reply, sizeof(reply), 0);
                if (n == 0) break;
                if (n < 0) { CHECK(socket_get_error() == ERR_WOULDBLOCK); pump(); continue; }
                for (ssize_t j = 0; j < n; j++, received++) CHECK((uint8_t)reply[j] == received % 251);
            }
            CHECK(received == TCP_PROXY_WINDOW && recv(fd, reply, sizeof(reply), 0) == 0);
        } else {
            test_clock_run_for(ua, 60000);
            dispatch(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, cmd, sid, NULL, 0);
            test_clock_run_for(ua, 38000);
            CHECK(p->socks_conn_count + p->http_conn_count == 1);
            test_clock_run_for(ua, 2500);
        }
        CHECK(!p->socks_conn_count && !p->http_conn_count && !p->terminal_count);
        CHECK(count_cmd(TCP_PROXY_SUBCMD_ERROR) == (unsigned)(mode == 1));
        socket_close_wrapper(fd);
    }
    tcp_proxy_client_destroy(p); inst->tcp_proxy_client = NULL; pump();
    puts("[PASS] SOCKS CLOSE and CONNECT FIN drain expire at 10s, half-close survives, HTTP 101 delivers all queued bytes before EOF");
}

static void http_tests(struct UTUN_INSTANCE* inst) {
    struct tcp_proxy_client* p = tcp_proxy_client_create(inst, ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed, 0, NULL, 1, "127.0.0.1:0");
    CHECK(p); inst->tcp_proxy_client = p;
    uint16_t port = socks_proxy_get_port(p->http_listen); CHECK(port);
    char out[64000]; socket_t fd = connect_local(port); message_count = 0;
    const char pipeline[] = "GET http://127.0.0.1:8080?x=1 HTTP/1.1\r\nHost: wrong\r\n"
                            "Proxy-Authorization: secret\r\nConnection: X-Hop\r\nX-Hop: secret\r\n\r\n"
                            "GET http://127.0.0.2:8081/b HTTP/1.1\r\nHost: wrong\r\n\r\n";
    send_bytes(fd, pipeline, sizeof(pipeline)-1);
    uint32_t first = p->http_conns->stream_id;
    check_before_connected(inst, &p->http_conns->flow);
    CHECK(count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 1);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    CHECK(data_sent(first, out, sizeof(out)) > 0 && strstr(out, "GET /?x=1 HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"));
    CHECK(!strstr(out, "secret") && !strstr(out, "/b"));
    const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close, X-Hop\r\nX-Hop: secret\r\n\r\none";
    // CLOSE сразу после полного response не должен оборвать ещё не записанные в OS данные.
    CHECK(socks_proxy_handle_etcp(&p->http_conns, &p->http_conn_count, first,
                                  TCP_PROXY_SUBCMD_DATA, (const uint8_t*)reply, sizeof(reply)-1) == 1);
    CHECK(p->http_conns->stream_id == first && p->http_conns->tc->write_queue->head);
    CHECK(socks_proxy_handle_etcp(&p->http_conns, &p->http_conn_count, first, TCP_PROXY_SUBCMD_CLOSE, NULL, 0) == 1);
    reject_retire = 1;
    pump();
    CHECK(p->http_conns->stream_id == first && count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 1);
    reject_retire = 0;
    uint32_t credit = 1;
    CHECK(socks_proxy_handle_etcp(&p->http_conns, &p->http_conn_count, first,
                                  TCP_PROXY_SUBCMD_WINDOW, (uint8_t*)&credit, 4) == 1);
    test_clock_run_for(ua, 750);
    CHECK(p->http_conn_count == 1 && count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 2);
    uint32_t second = p->http_conns->stream_id; CHECK(second && first != second);
    CHECK(messages[message_count-1].cmd == TCP_PROXY_SUBCMD_CONNECT);
    CHECK(messages[message_count-1].data[3] == 2);
    CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "\r\n\r\none") && !strstr(out, "secret"));
    CHECK(!strstr(out, "Connection: close"));
    CHECK(socks_proxy_handle_etcp(&p->http_conns, &p->http_conn_count, first, TCP_PROXY_SUBCMD_FIN, NULL, 0) == 0);
    CHECK(socks_proxy_handle_etcp(&p->http_conns, &p->http_conn_count, first, TCP_PROXY_SUBCMD_WINDOW, (uint8_t*)&credit, 4) == 0);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, second, NULL, 0);
    CHECK(data_sent(second, out, sizeof(out)) > 0 && strstr(out, "GET /b HTTP/1.1\r\nHost: 127.0.0.2:8081\r\n"));
    upstream(inst, second, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo");
    CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "\r\n\r\ntwo"));
    // Следующий GET приходит только после чтения второго ответа, когда входная очередь опустела.
    const char next_get[] = "GET http://127.0.0.1/next HTTP/1.1\r\nHost: x\r\n\r\n";
    send_bytes(fd, next_get, sizeof(next_get)-1);
    CHECK(count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 3);
    uint32_t third = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, third, NULL, 0);
    const char chunk_header[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    dispatch(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_DATA, third, chunk_header, sizeof(chunk_header)-1);
    for (int i = 0; i < 100; i++) dispatch(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_DATA, third, "1\r\nx\r\n", 6);
    dispatch(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_DATA, third, "0\r\n\r\n", 5);
    struct ll_queue* writes = p->http_conns->tc->write_queue;
    CHECK(writes->count == 1 && queue_total_bytes(writes) == writes->head->len);
    size_t wire_len = queue_total_bytes(writes);
    pump();
    CHECK(client_reply(fd, out, sizeof(out)) == wire_len && strstr(out, "0\r\n\r\n"));
    socket_close_wrapper(fd); pump(); CHECK(p->http_conn_count == 0);

    // EOF в незавершённых заголовках и теле освобождает frontend, вместо вечного ожидания.
    fd = connect_local(port); message_count = 0;
    const char truncated[] = "GET http://127.0.0.1/ HTTP/1.1\r\nHost:";
    send_bytes(fd, truncated, sizeof(truncated)-1);
    CHECK(shutdown(fd, SHUT_WR) == 0); pump();
    CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "HTTP/1.1 400"));
    CHECK(p->http_conn_count == 0 && count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 0); socket_close_wrapper(fd);

    const char post[] = "POST http://127.0.0.1/ HTTP/1.1\r\nHost: x\r\nContent-Length: 9\r\nExpect: 100-continue\r\n\r\n";
    fd = connect_local(port); message_count = 0; send_bytes(fd, post, sizeof(post)-1);
    first = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    upstream(inst, first, "HTTP/1.1 100 Continue\r\n\r\n");
    CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "100 Continue"));
    send_bytes(fd, "123456789", 9);
    CHECK(data_sent(first, out, sizeof(out)) > 9 && !strcmp(out+strlen(out)-9, "123456789"));
    upstream(inst, first, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: X-Hop\r\n\r\n");
    const char chunks[] = "3\r\nabc\r\n0\r\nX-Hop: secret\r\nX-End: yes\r\n\r\n";
    for (size_t i = 0; i < sizeof(chunks)-1; i++)
        deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_DATA, first, chunks+i, 1);
    CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "3\r\nabc\r\n0\r\nX-End: yes\r\n\r\n"));
    CHECK(!strstr(out, "secret"));
    uint32_t returned = 0;
    for (unsigned i = 0; i < message_count; i++) if (messages[i].sid == first && messages[i].cmd == TCP_PROXY_SUBCMD_WINDOW) {
        uint32_t n; memcpy(&n, messages[i].data, 4); returned += n;
    }
    CHECK(returned == strlen("HTTP/1.1 100 Continue\r\n\r\n") +
                      strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: X-Hop\r\n\r\n") + strlen(chunks));
    socket_close_wrapper(fd); pump(); CHECK(p->http_conn_count == 0);

    // Ранний final и malformed response синхронно внутри отправки request headers.
    for (int which = 1; which <= 2; which++) {
        fd = connect_local(port); message_count = 0; send_bytes(fd, post, sizeof(post)-1);
        first = p->http_conns->stream_id; immediate_response = which;
        deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
        CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, which == 1 ? "HTTP/1.1 413" : "HTTP/1.1 502"));
        CHECK(p->http_conn_count == 0 && count_cmd(TCP_PROXY_SUBCMD_ERROR) >= 1);
        socket_close_wrapper(fd);
    }
    fd = connect_local(port); message_count = 0; send_bytes(fd, post, sizeof(post)-1);
    first = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    send_bytes(fd, "abc", 3); CHECK(shutdown(fd, SHUT_WR) == 0); pump();
    CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "HTTP/1.1 400"));
    CHECK(p->http_conn_count == 0); socket_close_wrapper(fd);

    // Close-delimited response завершается только FIN и закрывает frontend.
    const char get[] = "GET http://127.0.0.1/ HTTP/1.1\r\nHost: x\r\n\r\n";
    fd = connect_local(port); message_count = 0; send_bytes(fd, get, sizeof(get)-1);
    first = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    upstream(inst, first, "HTTP/1.1 200 OK\r\n\r\nbody"); CHECK(p->http_conn_count == 1);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_FIN, first, NULL, 0);
    CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "Connection: close") && strstr(out, "\r\n\r\nbody"));
    CHECK(p->http_conn_count == 0); socket_close_wrapper(fd);

    // Полностью принятый ответ с Connection: close получает тот же срок дописывания.
    fd = connect_local(port); message_count = 0;
    const char close_get[] = "GET http://127.0.0.1/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    send_bytes(fd, close_get, sizeof(close_get)-1); first = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    queue_set_callback(p->http_conns->tc->write_queue, NULL, NULL);
    upstream(inst, first, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody");
    CHECK(p->http_conn_count == 1 && uasync_timer_armed(p->http_conns->drain_timer));
    test_clock_run_for(ua, 98000); CHECK(p->http_conn_count == 1);
    test_clock_run_for(ua, 2500); CHECK(!p->http_conn_count && count_cmd(TCP_PROXY_SUBCMD_ERROR) == 1);
    socket_close_wrapper(fd);

    // HEAD не ждёт объявленное Content-Length тело; pipeline сохраняется и после клиентского EOF.
    fd = connect_local(port); message_count = 0;
    const char heads[] = "HEAD http://127.0.0.1/ HTTP/1.1\r\nHost: x\r\n\r\n"
                         "GET http://127.0.0.2/ HTTP/1.1\r\nHost: x\r\n\r\n";
    send_bytes(fd, heads, sizeof(heads)-1); CHECK(shutdown(fd, SHUT_WR) == 0); pump();
    first = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    upstream(inst, first, "HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n");
    CHECK(p->http_conn_count == 1 && count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 2);
    second = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, second, NULL, 0);
    upstream(inst, second, "HTTP/1.1 204 No Content\r\n\r\n");
    CHECK(p->http_conn_count == 0 && client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "204 No Content"));
    socket_close_wrapper(fd);

    // HTTP/1.0 получает декодированное chunked тело и close; credits не зависят от размера результата.
    fd = connect_local(port); message_count = 0;
    const char old_get[] = "GET http://127.0.0.1/ HTTP/1.0\r\n\r\n";
    send_bytes(fd, old_get, sizeof(old_get)-1); first = p->http_conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    upstream(inst, first, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    upstream(inst, first, "3\r\nabc\r\n0\r\n\r\n");
    CHECK(p->http_conn_count == 0 && client_reply(fd, out, sizeof(out)) > 0);
    CHECK(strstr(out, "HTTP/1.0 200 OK") && strstr(out, "\r\n\r\nabc") && !strstr(out, "100 Continue"));
    CHECK(!strstr(out, "Transfer-Encoding")); socket_close_wrapper(fd);

    // Синхронный 101 видит отправленный последний header chunk; ранние raw bytes сохраняются.
    fd = connect_local(port); message_count = 0;
    const char upgrade[] = "GET http://127.0.0.1/ HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\nCLIENT";
    send_bytes(fd, upgrade, sizeof(upgrade)-1); first = p->http_conns->stream_id; immediate_response = 3;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, "101 Switching Protocols") && strstr(out, "\r\n\r\nRAW"));
    CHECK(data_sent(first, out, sizeof(out)) > 6 && !strcmp(out+strlen(out)-6, "CLIENT"));
    send_bytes(fd, "NEXT", 4); upstream(inst, first, "SERVER");
    CHECK(client_reply(fd, out, sizeof(out)) == 6 && !strcmp(out, "SERVER"));
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_ERROR, first, NULL, 0);
    socket_close_wrapper(fd); pump(); CHECK(p->http_conn_count == 0);

    // Закрытое окно возвращает снятый перед send chunk обратно в tx_buf без потери/дублирования.
    fd = connect_local(port); message_count = 0; send_bytes(fd, get, sizeof(get)-1); first = p->http_conns->stream_id;
    p->http_conns->flow.tx_credit = 0;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, first, NULL, 0);
    CHECK(count_cmd(TCP_PROXY_SUBCMD_DATA) == 0 && p->http_conns->tx_buf);
    credit = TCP_PROXY_WINDOW;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_WINDOW, first, &credit, 4);
    CHECK(count_cmd(TCP_PROXY_SUBCMD_DATA) == 1 && data_sent(first, out, sizeof(out)) > 0);
    CHECK(strstr(out, "GET / HTTP/1.1\r\nHost: 127.0.0.1:80\r\n") && !p->http_conns->tx_buf);
    upstream(inst, first, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    socket_close_wrapper(fd); pump(); CHECK(p->http_conn_count == 0);

    // Ограниченные по времени headers/connect и принудительное завершение зависшего drain.
    short_http_deadline = 1;
    for (int phase = 0; phase < 3; phase++) {
        fd = connect_local(port); message_count = 0;
        if (phase == 2) queue_set_callback(p->http_conns->tc->write_queue, NULL, NULL);
        send_bytes(fd, phase == 1 ? get : "GET", phase == 1 ? sizeof(get)-1 : 3);
        for (int i = 0; i < 22; i++) pump();
        if (phase == 2) {
            CHECK(p->http_conn_count == 1 && uasync_timer_armed(p->http_conns->drain_timer));
            test_clock_run_for(ua, 100000);
        }
        CHECK(p->http_conn_count == 0);
        if (phase == 1) CHECK(count_cmd(TCP_PROXY_SUBCMD_CONNECT) == 1 && count_cmd(TCP_PROXY_SUBCMD_ERROR) == 1);
        if (phase != 2) CHECK(client_reply(fd, out, sizeof(out)) > 0 && strstr(out, phase == 1 ? "504" : "408"));
        socket_close_wrapper(fd);
    }
    short_http_deadline = 0;
    tcp_proxy_client_destroy(p); inst->tcp_proxy_client = NULL; pump();
    puts("[PASS] HTTP keep-alive origins, pipeline, EOF, Continue/chunks/credits, reentrancy and deadlines");
}

static void server_tests(struct UTUN_INSTANCE* inst) {
    uint16_t port; socket_t listener = listen_local(&port);
    inst->tcp_proxy_server.enabled = 1; inst->tcp_proxy_server.inst = inst;
    uint8_t connect_data[6] = {127,0,0,1}; uint16_t wire_port = htons(port); memcpy(connect_data+4, &wire_port, 2);
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CONNECT, 2, connect_data, 6);
    socket_t a = accept(listener, NULL, NULL); CHECK(a != SOCKET_INVALID); socket_set_nonblocking(a);
    deliver(inst, 22, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CONNECT, 2, connect_data, 6);
    socket_t b = accept(listener, NULL, NULL); CHECK(b != SOCKET_INVALID); socket_set_nonblocking(b);
    CHECK(inst->tcp_proxy_server.conn_count == 2);
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_DATA, 2, "AAA", 3);
    deliver(inst, 22, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_DATA, 2, "BBB", 3);
    uint8_t buf[16]; CHECK(recv(a, buf, sizeof(buf), 0) == 3 && memcmp(buf, "AAA", 3) == 0);
    CHECK(recv(b, buf, sizeof(buf), 0) == 3 && memcmp(buf, "BBB", 3) == 0);
    deliver(inst, 33, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CLOSE, 2, NULL, 0);
    CHECK(inst->tcp_proxy_server.conn_count == 2);
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CONNECT, 2, connect_data, 6);
    CHECK(inst->tcp_proxy_server.conn_count == 2);
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CLOSE, 2, NULL, 0);
    CHECK(inst->tcp_proxy_server.conn_count == 1);
    deliver(inst, 22, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CLOSE, 2, NULL, 0);
    CHECK(inst->tcp_proxy_server.conn_count == 0);
    // Сначала FIN от ETCP, затем FIN из OS: on_fin закрывает и освобождает exit-поток.
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CONNECT, 3, connect_data, 6);
    socket_t c = accept(listener, NULL, NULL); CHECK(c != SOCKET_INVALID); socket_set_nonblocking(c);
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_FIN, 3, NULL, 0);
    CHECK(inst->tcp_proxy_server.conn_count == 1 && recv(c, buf, sizeof(buf), 0) == 0);
    message_count = 0;
    CHECK(shutdown(c, SHUT_WR) == 0); pump();
    CHECK(inst->tcp_proxy_server.conn_count == 0 && count_cmd(TCP_PROXY_SUBCMD_FIN) == 1);
    socket_close_wrapper(c);
    // ERROR уничтожает exit сразу, не доставляя отложенный upload после раннего отказа origin.
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CONNECT, 4, connect_data, 6);
    c = accept(listener, NULL, NULL); CHECK(c != SOCKET_INVALID); socket_set_nonblocking(c);
    struct tcp_proxy_server_conn* rc = tcp_proxy_server_find_conn(&inst->tcp_proxy_server, TOPO_GROUP_UTUN, (uint8_t[32]){11}, 4); CHECK(rc);
    queue_set_callback(rc->tc->write_queue, NULL, NULL);
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_DATA, 4, "discard", 7);
    CHECK(rc->tc->write_queue->count == 1);
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_ERROR, 4, NULL, 0);
    CHECK(inst->tcp_proxy_server.conn_count == 0 && recv(c, buf, sizeof(buf), 0) == 0); socket_close_wrapper(c);
    // Синхронная отмена внутри exit DATA send не освобождает текущий callback/пулы преждевременно.
    deliver(inst, 11, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CONNECT, 5, connect_data, 6);
    c = accept(listener, NULL, NULL); CHECK(c != SOCKET_INVALID); socket_set_nonblocking(c);
    abort_exit_data = 1; send_bytes(c, "response", 8);
    CHECK(!abort_exit_data && inst->tcp_proxy_server.conn_count == 0); socket_close_wrapper(c);
    socket_close_wrapper(a); socket_close_wrapper(b); socket_close_wrapper(listener);
    inst->tcp_proxy_server.enabled = 0; pump();
    puts("[PASS] exit streams isolated by node + stream ID, duplicate CONNECT rejected");
}

// Один и тот же peer/sid в UTUN и CHAT должен получить два независимых TCP-сокета.
static void group_stream_tests(struct UTUN_INSTANCE* inst) {
    uint64_t groups[] = {111, TOPO_GROUP_UTUN};
    uint16_t port; socket_t listener = listen_local(&port);
    uint8_t target[7] = {127,0,0,1}; port = htons(port); memcpy(target + 4, &port, 2);
    inst->tcp_proxy_server.enabled = 1;
    chat_admitted = 1; message_count = 0;
    const uint8_t svc = ETCP_RT_ID_TCP_PROXY_SERVER;
    dispatch_group(inst, 111, 11, svc, TCP_PROXY_SUBCMD_CONNECT, 1, target, 6);
    CHECK(!inst->tcp_proxy_server.conns && !message_count);
    uint8_t keys[2][32] = {{0}}; memcpy(keys[0], exit_ed, 32);
    inst->config->global.tcp_proxy_groups = keys; inst->config->global.tcp_proxy_group_count = 2;
    dispatch_group(inst, 111, 22, svc, TCP_PROXY_SUBCMD_CONNECT, 1, target, 6);
    dispatch_group(inst, 112, 11, svc, TCP_PROXY_SUBCMD_CONNECT, 1, target, 6);
    dispatch_group(inst, 111, 11, svc, TCP_PROXY_SUBCMD_CONNECT, 1, target, 7);
    CHECK(!inst->tcp_proxy_server.conns && !message_count);
    for (size_t i = 0; i < 2; i++) {
        dispatch_group(inst, groups[i], 11, svc, TCP_PROXY_SUBCMD_CONNECT, 1, target, 6); pump();
        CHECK(message_count == i + 1 && messages[i].group == groups[i] && messages[i].cmd == TCP_PROXY_SUBCMD_CONNECTED);
    }
    socket_t chat = accept(listener, NULL, NULL), utun = accept(listener, NULL, NULL);
    CHECK(chat != SOCKET_INVALID && utun != SOCKET_INVALID);
    CHECK(!socket_set_nonblocking(chat) && !socket_set_nonblocking(utun));
    struct tcp_proxy_server_conn* rc = tcp_proxy_server_find_conn(&inst->tcp_proxy_server, 111, (uint8_t[32]){11}, 1); CHECK(rc);
    uint8_t data[TCP_PROXY_CHUNK + 1] = {0};
    const struct { uint8_t cmd; size_t length; } bad[] = {
        {TCP_PROXY_SUBCMD_CONNECT, 5}, {TCP_PROXY_SUBCMD_CONNECTED, 0}, {TCP_PROXY_SUBCMD_WINDOW, 3}, {TCP_PROXY_SUBCMD_WINDOW, 5},
        {TCP_PROXY_SUBCMD_FIN, 1}, {TCP_PROXY_SUBCMD_CLOSE, 1}, {TCP_PROXY_SUBCMD_ERROR, 1},
        {TCP_PROXY_SUBCMD_DATA, 0}, {TCP_PROXY_SUBCMD_DATA, sizeof(data)}, {255, 0}
    };
    for (size_t n = 0; n < sizeof(bad) / sizeof(*bad); n++) {
        dispatch_group(inst, 111, 11, svc, bad[n].cmd, 1, data, bad[n].length);
        CHECK(tcp_proxy_server_find_conn(&inst->tcp_proxy_server, 111, (uint8_t[32]){11}, 1) == rc);
        CHECK(!rc->flow.fin_received && !rc->cli_closed && rc->flow.rx_credit == TCP_PROXY_WINDOW && message_count == 2);
    }
    dispatch_group(inst, 111, 11, svc, TCP_PROXY_SUBCMD_DATA, 1, "C", 1);
    dispatch_group(inst, TOPO_GROUP_UTUN, 11, svc, TCP_PROXY_SUBCMD_DATA, 1, "U", 1); pump();
    CHECK(recv(chat, data, 1, 0) == 1 && data[0] == 'C'); CHECK(recv(utun, data, 1, 0) == 1 && data[0] == 'U');
    inst->config->global.tcp_proxy_max_flows_per_node = 2;
    dispatch_group(inst, TOPO_GROUP_UTUN, 11, svc, TCP_PROXY_SUBCMD_CONNECT, 2, target, 6);
    CHECK(inst->tcp_proxy_server.conn_count == 2 && count_cmd(TCP_PROXY_SUBCMD_ERROR) == 1);
    inst->config->global.tcp_proxy_max_flows_per_node = PROXY_DEFAULT_MAX_FLOWS_PER_NODE;
    message_count = 0; send_bytes(chat, "c", 1); send_bytes(utun, "u", 1);
    CHECK(message_count == 2 && messages[0].cmd == TCP_PROXY_SUBCMD_DATA && messages[0].group == 111 && messages[0].data[0] == 'c');
    CHECK(messages[1].cmd == TCP_PROXY_SUBCMD_DATA && messages[1].group == TOPO_GROUP_UTUN && messages[1].data[0] == 'u');
    dispatch_group(inst, 112, 11, svc, 0, 0, NULL, 0); CHECK(inst->tcp_proxy_server.conn_count == 2);
    chat_admitted = 0;
    dispatch_group(inst, 111, 11, svc, 0, 0, NULL, 0); CHECK(inst->tcp_proxy_server.conn_count == 1);
    dispatch_group(inst, TOPO_GROUP_UTUN, 11, svc, TCP_PROXY_SUBCMD_DATA, 1, "X", 1); pump();
    CHECK(recv(utun, data, 1, 0) == 1 && data[0] == 'X');
    dispatch_group(inst, TOPO_GROUP_UTUN, 11, svc, 0, 0, NULL, 0);
    CHECK(!inst->tcp_proxy_server.conns);
    socket_close_wrapper(chat); socket_close_wrapper(utun); socket_close_wrapper(listener);
    inst->tcp_proxy_server.enabled = 0;
    inst->config->global.tcp_proxy_groups = NULL; inst->config->global.tcp_proxy_group_count = 0;
    pump(); puts("[PASS] TCP group isolation, scoped close after revocation and command sizes");
}

// Одинаковые адреса и порты в двух группах. Для ICMP используется штатное тестовое эхо.
static void datagram_request(struct UTUN_INSTANCE* inst, uint64_t group, uint8_t svc, uint16_t port, int close_channel) {
    struct ll_entry* e = ll_alloc_lldgram(UDP_PROXY_RECV_HDR_SIZE + 1); CHECK(e);
    e->len = close_channel ? ROUTER_SVC_HDR_SIZE : UDP_PROXY_RECV_HDR_SIZE + 1;
    memset(e->dgram, 0, e->len);
    const uint8_t peer[32] = {11};
    uint32_t ip = htonl(INADDR_LOOPBACK);
    e->dgram[0] = svc;
    memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &group, 8); memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, peer, 32);
    if (!close_channel) {
        uint8_t* body = e->dgram + ROUTER_SVC_PAYLOAD_OFF;
        body[0] = 1; memcpy(body + 1, &ip, 4);
        if (svc == ETCP_RT_ID_UDP_PROXY) { memcpy(body + 5, &port, 2); memcpy(body + 7, &ip, 4); memcpy(body + 11, &port, 2); }
        else { memcpy(body + 5, &ip, 4); memcpy(body + 9, &port, 2); }
        body[13] = 'a';
    }
    struct ETCP_CONN conn = {.instance = inst};
    if (svc == ETCP_RT_ID_UDP_PROXY) udp_proxy_recv_cb(&conn, e);
    else icmp_proxy_recv_cb(&conn, e);
}

static void group_datagram_tests(struct UTUN_INSTANCE* inst) {
    uint64_t groups[] = {111, TOPO_GROUP_UTUN};
    uint8_t keys[2][32] = {{0}}; memcpy(keys[0], exit_ed, 32);
    inst->config->global.tcp_proxy_groups = keys; inst->config->global.tcp_proxy_group_count = 2;
    inst->tcp_proxy_server.enabled = 1; chat_admitted = 1; message_count = 0;
    CHECK(udp_proxy_init(inst, ua) == 0 && icmp_proxy_init(inst, ua) == 0);
    icmp_proxy_set_test_loopback(inst, 1);
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd != SOCKET_INVALID && !socket_set_nonblocking(fd));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    uint16_t port = htons(socket_get_bound_port(fd)); CHECK(port);
    for (size_t i = 0; i < 2; i++) {
        datagram_request(inst, groups[i], ETCP_RT_ID_UDP_PROXY, port, 0);
        struct sockaddr_in from; socklen_t size = sizeof(from); uint8_t data;
        CHECK(recvfrom(fd, &data, 1, 0, (struct sockaddr*)&from, &size) == 1 && data == 'a');
        CHECK(sendto(fd, &data, 1, 0, (struct sockaddr*)&from, size) == 1); pump();
        CHECK(message_count == 2*i+1 && messages[2*i].svc == ETCP_RT_ID_UDP_PROXY && messages[2*i].group == groups[i]);
        datagram_request(inst, groups[i], ETCP_RT_ID_ICMP_PROXY, port, 0);
        CHECK(message_count == 2*i+2 && messages[2*i+1].svc == ETCP_RT_ID_ICMP_PROXY && messages[2*i+1].group == groups[i]);
    }
    CHECK(inst->udp_proxy->flow_count == 2);
    chat_admitted = 0;
    datagram_request(inst, 111, ETCP_RT_ID_UDP_PROXY, port, 1);
    CHECK(inst->udp_proxy->flow_count == 1 && inst->udp_proxy->flows->group == TOPO_GROUP_UTUN);
    datagram_request(inst, 111, ETCP_RT_ID_ICMP_PROXY, port, 0);
    datagram_request(inst, 111, ETCP_RT_ID_UDP_PROXY, port, 0);
    CHECK(message_count == 4 && inst->udp_proxy->flow_count == 1);
    datagram_request(inst, TOPO_GROUP_UTUN, ETCP_RT_ID_UDP_PROXY, port, 1);
    CHECK(!inst->udp_proxy->flows && !inst->udp_proxy->expire_timer);
    socket_close_wrapper(fd); icmp_proxy_destroy(inst); udp_proxy_destroy(inst);
    inst->tcp_proxy_server.enabled = 0;
    inst->config->global.tcp_proxy_groups = NULL; inst->config->global.tcp_proxy_group_count = 0;
    pump(); puts("[PASS] UDP tuple isolated by group, UDP and ICMP replies return to the requesting group");
}

static void instance_tests(struct UTUN_INSTANCE* a) {
    struct UTUN_INSTANCE* b = u_calloc(1, sizeof(*b)); CHECK(b); b->ua = ua;
    CHECK(udp_proxy_init(a, ua) == 0 && udp_proxy_init(b, ua) == 0);
    CHECK(icmp_proxy_init(a, ua) == 0 && icmp_proxy_init(b, ua) == 0);
    CHECK(a->udp_proxy != b->udp_proxy && a->icmp_proxy != b->icmp_proxy);
    struct udp_proxy_ctx* udp = b->udp_proxy; struct icmp_proxy_ctx* icmp = b->icmp_proxy;
    udp_proxy_destroy(a); icmp_proxy_destroy(a);
    CHECK(b->udp_proxy == udp && b->icmp_proxy == icmp);
    CHECK(udp_proxy_init(b, ua) == 0 && b->udp_proxy == udp);
    udp_proxy_destroy(b); icmp_proxy_destroy(b); u_free(b);
    puts("[PASS] UDP/ICMP contexts and destruction are instance-local");
}

// Принимающий callback lwIP запускаем с PCB установленного соединения; дальнейшие
// recv/abort/shutdown проходят через настоящие callbacks proxy и стек lwIP.
static struct tcp_pcb* accept_tun(struct tcp_proxy_client* p) {
    struct tcp_pcb* pcb = tcp_new(p->lwip); CHECK(pcb);
    pcb->state = ESTABLISHED; pcb->local_port = 80; pcb->remote_port = 12345;
    pcb->local_ip = htonl(0xc0000201); pcb->remote_ip = htonl(0x0a000002);
    pcb->next = p->lwip->active_pcbs; p->lwip->active_pcbs = pcb;
    struct tcp_pcb_listen* listener = (struct tcp_pcb_listen*)p->lwip->listen_pcbs; CHECK(listener && listener->accept);
    CHECK(listener->accept(p, pcb, LERR_OK) == LERR_OK);
    return pcb;
}

// ACK от приложения проходит обычный вход lwIP, в том числе переход FIN_WAIT_1 → FIN_WAIT_2.
static void tun_segment(struct tcp_pcb* pcb, uint8_t flags, int with_data) {
    struct pbuf* pb = pbuf_alloc(PBUF_RAW, TCP_HLEN + with_data); CHECK(pb);
    struct tcp_hdr* hdr = pb->payload;
    memset(hdr, 0, pb->len);
    hdr->src = htons(pcb->remote_port); hdr->dest = htons(pcb->local_port);
    hdr->seqno = htonl(pcb->rcv_nxt); hdr->ackno = htonl(pcb->snd_nxt); hdr->wnd = htons(4096);
    TCPH_HDRLEN_FLAGS_SET(hdr, 5, flags);
    if (with_data) ((uint8_t*)pb->payload)[TCP_HLEN] = 'x';
    lwip_tcp_input(pcb->ctx, pb, pcb->remote_ip, pcb->local_ip);
}

static void tun_tests(struct UTUN_INSTANCE* inst) {
#ifndef _WIN32
    struct tcp_proxy_client_mapping_config map = {.local_port=80, .remote_port=80, .remote_ip="192.0.2.1"};
    struct tcp_proxy_client* p = tcp_proxy_client_create(inst, ua, "testproxy", "10.0.0.1", 1500, 1, &map, 1, exit_ed, 0, NULL, 0, NULL);
    CHECK(p); inst->tcp_proxy_client = p;
    message_count = 0;
    struct tcp_pcb* pcb = accept_tun(p); CHECK(p->conn_count == 1);
    uint32_t sid = p->conns->stream_id;
    check_before_connected(inst, &p->conns->flow);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, sid, NULL, 0);
    p->conns->flow.tx_credit = 0;
    struct pbuf* pb = pbuf_alloc(PBUF_RAW, 4); CHECK(pb); pbuf_take(pb, "tail", 4);
    CHECK(pcb->recv(pcb->callback_arg, pcb, pb, LERR_OK) == LERR_OK);
    pcb->state = CLOSE_WAIT;
    CHECK(pcb->recv(pcb->callback_arg, pcb, NULL, LERR_OK) == LERR_OK);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_FIN, sid, NULL, 0);
    CHECK(p->conn_count == 1 && p->conns->tx_queue->count == 1);
    message_count = 0; uint32_t credit = 4;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_WINDOW, sid, &credit, 4);
    CHECK(p->conn_count == 0 && count_cmd(TCP_PROXY_SUBCMD_DATA) == 1 && count_cmd(TCP_PROXY_SUBCMD_FIN) == 1);
    CHECK(messages[0].cmd == TCP_PROXY_SUBCMD_DATA && memcmp(messages[0].data, "tail", 4) == 0);
    pcb = accept_tun(p); CHECK(p->conn_count == 1);
    struct pbuf* pending = pbuf_alloc(PBUF_RAW, 1); CHECK(pending);
    pbuf_ref(pending); pcb->refused_data = pending;
    tcp_abort(pcb); CHECK(p->conn_count == 0);
    CHECK(pending->ref == 1); pbuf_free(pending);

    pcb = accept_tun(p); sid = p->conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, sid, NULL, 0);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_FIN, sid, NULL, 0);
    CHECK(pcb->state == FIN_WAIT_1);
    tun_segment(pcb, TCP_ACK, 0); CHECK(pcb->state == FIN_WAIT_2);
    message_count = 0;
    test_clock_run_for(ua, 150000);
    tun_segment(pcb, TCP_ACK, 1); pump(); CHECK(count_cmd(TCP_PROXY_SUBCMD_DATA) == 1);
    test_clock_run_for(ua, 150000); CHECK(p->conn_count == 1);
    tun_segment(pcb, TCP_ACK, 0); // Пустой ACK не продлевает ожидание.
    test_clock_run_for(ua, 80000);
    CHECK(!p->conn_count && count_cmd(TCP_PROXY_SUBCMD_ERROR) == 1);
    CHECK(memory_pool_is_freed(p->lwip->pcb_pool, pcb));
    puts("[PASS] FIN_WAIT_2 accepts reverse DATA, then releases idle PCB and exit stream");

    pcb = accept_tun(p); sid = p->conns->stream_id;
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, sid, NULL, 0);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_FIN, sid, NULL, 0);
    tun_segment(pcb, TCP_ACK, 0); CHECK(pcb->state == FIN_WAIT_2);
    p->conns->flow.tx_credit = 0; message_count = 0;
    tun_segment(pcb, TCP_ACK | TCP_FIN, 1);
    CHECK(pcb->state == TIME_WAIT && p->conn_count == 1 && !p->conns->pcb && p->conns->tx_queue->count == 1);
    deliver(inst, 42, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_WINDOW, sid, &credit, sizeof(credit));
    CHECK(!p->conn_count && message_count == 2 && messages[0].cmd == TCP_PROXY_SUBCMD_DATA && messages[1].cmd == TCP_PROXY_SUBCMD_FIN);

    // IPv4 options смещают UDP-заголовок; последующий фрагмент не является UDP-запросом.
    CHECK(udp_proxy_init(inst, ua) == 0); // Общий контекст создаёт владелец instance, не TCP client.
    uint8_t ip[36] = {0x46, 0, 0, 36, 0, 0, 0, 0, 64, 17};
    uint32_t src = htonl(0x0a000002), dst = htonl(0xc0000201);
    memcpy(ip+12, &src, 4); memcpy(ip+16, &dst, 4);
    uint16_t sport=htons(12345), dport=htons(53), ulen=htons(12);
    memcpy(ip+24, &sport, 2); memcpy(ip+26, &dport, 2); memcpy(ip+28, &ulen, 2); memcpy(ip+32, "data", 4);
    message_count = 0;
    const uint16_t lengths[] = {12, 12, 7, 13, 10, 8};
    for (unsigned i = 0; i < sizeof(lengths)/sizeof(lengths[0]); i++) {
        struct ll_entry* e = queue_entry_new(0); CHECK(e);
        e->dgram = u_calloc(1, sizeof(ip)+1); CHECK(e->dgram); e->len = sizeof(ip)+1;
        ip[7] = i == 1; ulen = htons(lengths[i]); memcpy(ip+28, &ulen, 2);
        memcpy(e->dgram+1, ip, sizeof(ip));
        queue_data_put(p->tun->output_queue, e); pump();
    }
    CHECK(message_count == 3 && messages[0].svc == ETCP_RT_ID_UDP_PROXY && messages[0].len == 12);
    CHECK(messages[1].len == 10 && !memcmp(messages[1].data+8, "da", 2) && messages[2].len == 8);
    CHECK(memcmp(messages[0].data+6, &dport, 2) == 0 && memcmp(messages[0].data+8, "data", 4) == 0);
    // Два свободных слота. Разные порты используют один listener, RST освобождает место до таймера.
    struct tcp_pcb* listener = p->lwip->listen_pcbs;
    CHECK(listener && !listener->next && !p->lwip->active_pcbs);
    p->socks_conn_count = TCP_PROXY_CLIENT_STREAM_LIMIT - 2;
    struct tcp_pcb incoming = {.ctx=p->lwip, .local_port=1200, .remote_port=43000, .remote_ip=src, .local_ip=dst};
    tun_segment(&incoming, TCP_SYN, 0);
    incoming.local_port++; tun_segment(&incoming, TCP_SYN, 0);
    CHECK(!tcp_proxy_client_can_accept(p));
    struct tcp_pcb* half = p->lwip->active_pcbs;
    CHECK(half && half->local_port == 1201 && half->next && !half->next->next);
    incoming.local_port++; tun_segment(&incoming, TCP_SYN, 0);
    CHECK(p->lwip->active_pcbs == half && p->lwip->listen_pcbs == listener && !listener->next);
    tun_segment(half, TCP_RST, 0); CHECK(tcp_proxy_client_can_accept(p));
    tun_segment(&incoming, TCP_SYN, 0); CHECK(p->lwip->active_pcbs->local_port == 1202);
    p->socks_conn_count = 0;
    tcp_proxy_client_destroy(p); inst->tcp_proxy_client = NULL; pump();
    CHECK(inst->udp_proxy != NULL); udp_proxy_destroy(inst);
    puts("[PASS] TUN pending DATA survives FIN; aborted PCB freed; IPv4 options/fragment validation");
#endif
}

static void local_tun_tests(struct UTUN_INSTANCE* inst) {
#ifndef _WIN32
    struct tcp_proxy_client_mapping_config map = {.local_port=80, .remote_port=80, .remote_ip="127.0.0.1"};
    struct tcp_proxy_client* p = tcp_proxy_client_create(inst, ua, "testproxy", "10.0.0.1", 1500, 1,
                                                         &map, 1, inst->my_ed25519_pubkey, 0, NULL, 0, NULL);
    CHECK(p); inst->tcp_proxy_client = p;
    struct tcp_pcb* pcb = accept_tun(p); pump();
    deliver(inst, inst->node_id, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_CONNECTED, p->conns->stream_id, NULL, 0);
    message_count = 0;
    uint8_t payload[TCP_PROXY_CHUNK + 1] = {0};
    struct pbuf* pb = pbuf_alloc(PBUF_RAW, sizeof(payload)); CHECK(pb);
    CHECK(pbuf_take(pb, payload, sizeof(payload)) == LERR_OK);
    CHECK(pcb->recv(pcb->callback_arg, pcb, pb, LERR_OK) == LERR_OK);
    CHECK(!message_count && p->conns->tx_queue->count == 2);
    data_reply = TCP_PROXY_SUBCMD_ERROR;
    pump();
    CHECK(!data_reply && !p->conn_count && !p->conns && !p->lwip->active_pcbs);
    CHECK(message_count == 1 && messages[0].cmd == TCP_PROXY_SUBCMD_DATA && messages[0].len == TCP_PROXY_CHUNK);
    tcp_proxy_client_destroy(p); inst->tcp_proxy_client = NULL; pump();
    puts("[PASS] local exit cancellation releases lwIP stream and remaining DATA after recv");
#endif
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_PROXY, DEBUG_LEVEL_DEBUG);
    test_clock_init(100);
    CHECK(socket_platform_init() == 0);
    ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE* inst = u_calloc(1, sizeof(*inst)); CHECK(inst);
    struct utun_config config = {0}; inst->config = &config; inst->ua = ua;
    memcpy(inst->my_ed25519_pubkey, self_ed, 32); inst->node_id = UINT64_C(0x1062abfb7d153935);
    struct TOPO_GROUP group = {.group_id=111, .group_type=TOPO_GROUP_TYPE_CHAT, .instance=inst};
    group.ll.size = sizeof(group) - sizeof(group.ll);
    sc_public_key_hex(exit_ed, group.channel_id);
    struct TOPO_GROUPS groups = {.instance=inst, .group_list=queue_new(ua, 16, 0, 8, "proxy fixture")};
    CHECK(groups.group_list && !queue_data_put_with_index(groups.group_list, &group.ll)); inst->topo_groups = &groups;
    instance_tests(inst); parser_tests(inst); http_tests(inst); tunnel_drain_tests(inst); server_tests(inst);
    group_stream_tests(inst); group_datagram_tests(inst); tun_tests(inst); local_tun_tests(inst); bounded_open_tests(inst);
    CHECK(queue_data_get(groups.group_list) == &group.ll); queue_free(groups.group_list);
    u_free(inst); pump(); uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == 0);
    socket_platform_cleanup();
    test_clock_finish();
    return 0;
}
