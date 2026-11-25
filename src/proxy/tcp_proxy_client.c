// tcp_proxy_client.c — TCP прокси-клиент: стек lwIP TCP → ETCP → удалённый exit узел
#include "tcp_proxy_client.h"
#include "socks_proxy.h"
#include "lwip_tcp/lwip_tcp.h"
#include "lwip_tcp/lwip_tcp_priv.h"
#include "lwip_tcp/lwip_tcp_opts.h"
#include "config_parser.h"
#include "tun_if.h"
#include "utun_instance.h"
#include "etcp.h"
#include "etcp_api.h"
#include "../routing_layer/topo_node.h"
#include "etcp_router.h"
#include "tcp_proxy_server.h"
#include "udp_proxy.h"
#include "icmp_proxy.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/mem.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#endif

#ifndef INADDR_ANY
#define INADDR_ANY 0
#endif

// ====================================================================
// Предварительные объявления
// ====================================================================
static void tcp_proxy_client_flow_wake(void* arg);
static int tcp_proxy_client_fin_flush(struct tcp_proxy_client_conn* pc);
static err_t tcp_proxy_client_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static err_t tcp_proxy_client_sent_cb(void *arg, struct tcp_pcb *pcb, uint16_t len);
static void  tcp_proxy_client_err_cb(void *arg, err_t err);
static err_t tcp_proxy_client_poll_cb(void *arg, struct tcp_pcb *pcb);
static err_t tcp_proxy_client_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_proxy_client_output_cb(void *arg, struct pbuf *p, uint32_t src_ip, uint32_t dst_ip);
static void  tcp_proxy_client_feed_from_transport(struct tcp_proxy_client_conn *pc);
static struct ll_entry* tcp_proxy_client_entry_from_data(struct memory_pool* pool, const uint8_t* data, uint16_t len);
static void  tcp_proxy_client_conn_free(struct tcp_proxy_client_conn *pc);
static void  tcp_proxy_client_conn_detach(struct tcp_proxy_client_conn *pc);
static void  tcp_proxy_client_abort_pcb(struct tcp_proxy_client_conn *pc, const char* why);
static int   tcp_proxy_client_send_msg(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t dst[32], uint8_t subcmd, uint32_t sid, const uint8_t* data, size_t len, int force);
static struct tcp_proxy_client_conn* tcp_proxy_client_find_conn(struct tcp_proxy_client* p, uint32_t stream_id);
static void  tcp_proxy_client_tx_queue_drain_cb(struct ll_queue* q, void* arg);
static void tcp_proxy_client_tx_run(void* arg);

// Общий диспетчер отправки на один exit. Буферы DATA остаются у потоков.
static struct ETCP_ROUTER_CONN* tcp_proxy_client_route(struct tcp_proxy_client* p) {
    return etcp_router_conn_lookup(p->inst, TOPO_GROUP_UTUN, p->via_ed, ETCP_RT_ID_TCP_PROXY_SERVER);
}

static int tcp_proxy_client_is_local(const struct tcp_proxy_client* p) {
    return p->inst && !memcmp(p->via_ed, p->inst->my_ed25519_pubkey, 32);
}

static int tcp_proxy_client_depth(struct tcp_proxy_client* p) {
    struct ETCP_ROUTER_CONN* route = tcp_proxy_client_route(p);
    return route && route->send_q ? (int)queue_entry_count(route->send_q) : 0;
}

int tcp_proxy_client_can_accept(const struct tcp_proxy_client* p) {
    int used = p->conn_count + p->socks_conn_count + p->http_conn_count + p->terminal_count;
    for (const struct tcp_pcb* pcb = p->lwip ? p->lwip->active_pcbs : NULL; pcb; pcb = pcb->next)
        if (pcb->state == SYN_RCVD) used++;
    return used < TCP_PROXY_CLIENT_STREAM_LIMIT;
}

int tcp_proxy_client_send_check(struct UTUN_INSTANCE* inst, const uint8_t peer[32], uint8_t cmd) {
    struct tcp_proxy_client* p = inst ? inst->tcp_proxy_client : NULL;
    if (!p || memcmp(p->via_ed, peer, 32) || tcp_proxy_client_is_local(p)) return 0;
    int depth = tcp_proxy_client_depth(p);
    if (cmd == TCP_PROXY_SUBCMD_CONNECT || cmd == TCP_PROXY_SUBCMD_DATA) {
        if (depth >= ROUTER_MAX_SEND_Q_PACKETS || (!p->tx_running &&
            (p->tx_tasks[PROXY_TX_CONNECT]->head || p->tx_tasks[PROXY_TX_DATA]->head))) return PROXY_SEND_BUSY;
    } else if (depth >= ROUTER_MAX_SEND_Q_PACKETS + TCP_PROXY_CLIENT_CONTROL_RESERVE ||
               (!p->tx_running && (p->terminal_count || p->tx_tasks[PROXY_TX_CONTROL]->head))) return PROXY_SEND_BUSY;
    return 0;
}

static void tcp_proxy_client_tx_ready(struct ll_queue* q, void* arg) {
    (void)q;
    struct tcp_proxy_client* p = arg;
    uasync_schedule_work(p->ua, &p->tx_kick, tcp_proxy_client_tx_run, p);
}

void tcp_proxy_client_schedule(struct tcp_proxy_client* p, struct proxy_tx_task* task, int kind, void (*run)(void*), void* arg) {
    if (task->link.owner) return;
    task->run = run; task->arg = arg;
    queue_data_put(p->tx_tasks[kind], &task->link);
    if (!p->tx_running && !uasync_timer_armed(p->tx_retry) && !p->tx_waiter.internal && !p->tx_waiter.call_soon_id) tcp_proxy_client_tx_ready(NULL, p);
}

static void tcp_proxy_client_tx_retry(void* arg) {
    struct tcp_proxy_client* p = arg;
    tcp_proxy_client_tx_ready(NULL, p);
}

// Один отложенный проход, FIFO внутри класса. CONNECT и DATA чередуются при одновременном ожидании.
static void tcp_proxy_client_tx_run(void* arg) {
    struct tcp_proxy_client* p = arg;
    uasync_timer_set(p->tx_retry, -1);
    p->tx_running = 1;
    int before = tcp_proxy_client_depth(p);
    int remaining[PROXY_TX_TASKS];
    for (int kind = 0; kind < PROXY_TX_TASKS; kind++) remaining[kind] = p->tx_tasks[kind]->count;
    if (p->terminal_count && get_time_tb() >= p->terminals[p->terminal_head].deadline_tb) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy TX retirement timeout peer=%s pending=%u", sc_public_key_hex(p->via_ed, (char[65]){0}), p->terminal_count);
        p->terminal_count = 0;
        etcp_router_conn_close(tcp_proxy_client_route(p));
    }
    unsigned terminals = p->terminal_count;
    for (int i = 0; i < ROUTER_MAX_SEND_Q_PACKETS; i++) {
        int depth = tcp_proxy_client_depth(p);
        if (depth >= ROUTER_MAX_SEND_Q_PACKETS + TCP_PROXY_CLIENT_CONTROL_RESERVE) break;
        if (terminals && p->terminal_count) {
            struct proxy_terminal message = p->terminals[p->terminal_head];
            if (proxy_send_packet(p->inst, TOPO_GROUP_UTUN, p->via_ed, ETCP_RT_ID_TCP_PROXY_SERVER, message.cmd, message.sid, NULL, 0, 1) < 0) break;
            p->terminal_head = (p->terminal_head + 1) % TCP_PROXY_CLIENT_STREAM_LIMIT;
            p->terminal_count--; terminals--;
            continue;
        }
        int kind = PROXY_TX_CONTROL;
        if (!remaining[kind] || !p->tx_tasks[kind]->head) {
            if (depth >= ROUTER_MAX_SEND_Q_PACKETS) break;
            kind = p->next_open ? PROXY_TX_CONNECT : PROXY_TX_DATA;
            if (!remaining[kind] || !p->tx_tasks[kind]->head) kind = PROXY_TX_CONNECT + PROXY_TX_DATA - kind;
            if (!remaining[kind] || !p->tx_tasks[kind]->head) break;
            p->next_open = kind == PROXY_TX_DATA;
        }
        remaining[kind]--;
        struct proxy_tx_task* task = (struct proxy_tx_task*)queue_data_get(p->tx_tasks[kind]);
        task->run(task->arg);
    }
    p->tx_running = 0;
    unsigned pending = p->terminal_count;
    for (int kind = 0; kind < PROXY_TX_TASKS; kind++) pending += p->tx_tasks[kind]->count;
    if (!pending) return;
    int depth = tcp_proxy_client_depth(p);
    if (depth >= ROUTER_MAX_SEND_Q_PACKETS && !p->tx_waiter.internal && !p->tx_waiter.call_soon_id)
        etcp_router_conn_on_send_ready(tcp_proxy_client_route(p), &p->tx_waiter, tcp_proxy_client_tx_ready, p);
    else if (depth > before) tcp_proxy_client_tx_ready(NULL, p);
    if (!p->tx_kick.ua) uasync_timer_set(p->tx_retry, 500);
}

void tcp_proxy_client_retire(struct tcp_proxy_client* p, uint32_t sid, uint8_t cmd) {
    if (!p->terminal_count && proxy_send(p->inst, TOPO_GROUP_UTUN, p->via_ed, ETCP_RT_ID_TCP_PROXY_SERVER, cmd, sid, NULL, 0, 1) == 0) return;
    if (p->terminal_count == TCP_PROXY_CLIENT_STREAM_LIMIT) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy TX retirement buffer full sid=%08x cmd=%u", sid, cmd);
        p->terminals[p->terminal_head].deadline_tb = 0; // закрытие канала вне стека освобождаемого потока
    } else {
        unsigned tail = (p->terminal_head + p->terminal_count++) % TCP_PROXY_CLIENT_STREAM_LIMIT;
        p->terminals[tail] = (struct proxy_terminal){get_time_tb() + 300000, sid, cmd};
    }
    if (!p->tx_running && !uasync_timer_armed(p->tx_retry) && !p->tx_waiter.internal) tcp_proxy_client_tx_ready(NULL, p);
}

void tcp_proxy_client_cancel_open(struct proxy_flow* f) {
    if (f->open_task.link.owner) queue_remove_data(f->open_task.link.owner, &f->open_task.link);
    if (f->open_timer) { uasync_cancel_timeout(f->ua, f->open_timer); f->open_timer = NULL; }
    if (f->opening) { f->opening = 0; f->client->opening--; }
}

void tcp_proxy_client_cancel_flow(struct proxy_flow* f) {
    tcp_proxy_client_cancel_open(f);
    if (f->data_task.link.owner) queue_remove_data(f->data_task.link.owner, &f->data_task.link);
    if (f->control_task.link.owner) queue_remove_data(f->control_task.link.owner, &f->control_task.link);
}

static void tcp_proxy_client_open_timeout(void* arg) {
    struct proxy_flow* f = arg;
    f->open_timer = NULL;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy CONNECT timeout peer=%s sid=%08x sent=%u",
               sc_public_key_hex(f->peer, (char[65]){0}), f->sid, f->connect_sent);
    tcp_proxy_client_cancel_open(f);
    f->open_failed(f->arg, ETIMEDOUT);
}

// Запускаем срок ответа до send: loopback может синхронно завершить и освободить поток.
static int tcp_proxy_client_submit_open(struct proxy_flow* f) {
    if (tcp_proxy_client_send_check(f->inst, f->peer, TCP_PROXY_SUBCMD_CONNECT)) return -EAGAIN;
    if (f->open_timer) uasync_cancel_timeout(f->ua, f->open_timer);
    f->open_timer = uasync_set_timeout(f->ua, 150000, f, tcp_proxy_client_open_timeout, "proxy_connected");
    if (!f->open_timer) return -ENOMEM;
    f->connect_sent = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy CONNECT submit peer=%s sid=%08x waiting=%u queue=%d",
                sc_public_key_hex(f->peer, (char[65]){0}), f->sid, f->client->opening, tcp_proxy_client_depth(f->client));
    if (proxy_send_packet(f->inst, f->group, f->peer, f->svc, TCP_PROXY_SUBCMD_CONNECT, f->sid, f->target, sizeof(f->target), 0) == 0) return 0;
    f->connect_sent = 0;
    return -EHOSTUNREACH;
}

static void tcp_proxy_client_open_retry(void* arg) {
    struct proxy_flow* f = arg;
    int ret = tcp_proxy_client_submit_open(f);
    if (ret == 0) return;
    if (ret == -EAGAIN) {
        tcp_proxy_client_schedule(f->client, &f->open_task, PROXY_TX_CONNECT, tcp_proxy_client_open_retry, f);
        return;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy deferred CONNECT failed peer=%s sid=%08x error=%d",
                sc_public_key_hex(f->peer, (char[65]){0}), f->sid, -ret);
    tcp_proxy_client_cancel_open(f);
    f->open_failed(f->arg, -ret);
}

int tcp_proxy_client_open(struct proxy_flow* f, const uint8_t ip[4], uint16_t port, void (*failed)(void*, int)) {
    struct tcp_proxy_client* p = f->client;
    if (f->inst && !etcp_router_peer_reachable(f->inst, f->group, f->peer, f->svc)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy CONNECT no live path peer=%s sid=%08x", sc_public_key_hex(f->peer, (char[65]){0}), f->sid);
        return -EHOSTUNREACH;
    }
    if (p->opening >= TCP_PROXY_CLIENT_OPEN_LIMIT) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy CONNECT capacity exhausted peer=%s sid=%08x pending=%u",
                   sc_public_key_hex(f->peer, (char[65]){0}), f->sid, p->opening);
        return -ENOBUFS;
    }
    f->opening = 1; p->opening++;
    f->open_failed = failed;
    memcpy(f->target, ip, 4); memcpy(f->target + 4, &port, 2);
    // TUN должен закончить accept до синхронного callback локального exit.
    int ret = tcp_proxy_client_is_local(p) ? -EAGAIN : tcp_proxy_client_submit_open(f);
    if (ret == 0) return 0;
    if (ret == -EAGAIN) {
        f->open_timer = uasync_set_timeout(f->ua, 50000, f, tcp_proxy_client_open_timeout, "proxy_open_slot");
        if (f->open_timer) {
            tcp_proxy_client_schedule(p, &f->open_task, PROXY_TX_CONNECT, tcp_proxy_client_open_retry, f);
            return 0;
        }
        ret = -ENOMEM;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy CONNECT failed peer=%s sid=%08x error=%d", sc_public_key_hex(f->peer, (char[65]){0}), f->sid, -ret);
    tcp_proxy_client_cancel_open(f);
    return ret;
}

// ====================================================================
// Помощник ll_entry
// ====================================================================
static struct ll_entry* tcp_proxy_client_entry_from_data(struct memory_pool* pool, const uint8_t* data, uint16_t len) {
    struct ll_entry* e = queue_entry_new_from_pool(pool);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "tcp_proxy_client_entry_from_data: pool exhausted"); return NULL; }
    e->len = 0; e->dgram = NULL;
    if (len > 0) {
        uint8_t* buf = u_malloc(len);
        if (!buf) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "tcp_proxy_client_entry_from_data: malloc(%u) failed", len); queue_entry_free(e); return NULL; }
        memcpy(buf, data, len); e->dgram = buf; e->len = len;
    }
    return e;
}

// ====================================================================
// Протокол: сборка и отправка сообщений прокси через ETCP
// ====================================================================
static int tcp_proxy_client_send_msg(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t dst[32], uint8_t subcmd,
                                    uint32_t sid, const uint8_t* data, size_t len, int force) {
    return proxy_send(inst, group_id, dst, ETCP_RT_ID_TCP_PROXY_SERVER, subcmd, sid, data, len, force);
}

static int tcp_proxy_client_send_close(struct tcp_proxy_client_conn* pc) {
    if (!pc->flow.connect_sent) return 0;
    DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "PROXY SEND sid=%08x", pc->stream_id);
    int ret = tcp_proxy_client_send_msg(pc->proxy->inst, TOPO_GROUP_UTUN, pc->proxy->via_ed,
                              TCP_PROXY_SUBCMD_CLOSE, pc->stream_id, NULL, 0, 1);
    if (ret < 0) { pc->close_pending = 1; return -1; }
    pc->close_pending = 0;
    pc->close_sent = 1;
    return 0;
}

static int tcp_proxy_client_send_error(struct tcp_proxy_client_conn* pc) {
    if (!pc->flow.connect_sent) return 0;
    int ret = tcp_proxy_client_send_msg(pc->proxy->inst, TOPO_GROUP_UTUN, pc->proxy->via_ed,
                              TCP_PROXY_SUBCMD_ERROR, pc->stream_id, NULL, 0, 1);
    if (ret < 0) { pc->close_pending = 1; return -1; }
    pc->close_pending = 0;
    pc->close_sent = 1;
    return 0;
}

// ====================================================================
// Вывод: lwIP TCP отправляет IP пакеты через этот callback
// ====================================================================
static err_t tcp_proxy_client_output_cb(void *arg, struct pbuf *p, uint32_t src_ip, uint32_t dst_ip) {
    struct tcp_proxy_client *proxy = (struct tcp_proxy_client *)arg;
    (void)src_ip; (void)dst_ip;
    uint16_t len = p->tot_len;
    if (len > TUN_MAX_PACKET_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy output too large: len=%u max=%d", len, TUN_MAX_PACKET_SIZE);
        return LERR_BUF;
    }
    uint8_t buf[TUN_MAX_PACKET_SIZE];
    if (proxy->tun) {
        pbuf_copy_partial(p, buf, len, 0);
        if (tun_write(proxy->tun, buf, len) != (ssize_t)len) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy TUN output failed: len=%u", len);
            return LERR_IF;
        }
    }
    return LERR_OK;
}

// ====================================================================
// Обработчик не-TCP (UDP/ICMP прокси)
// ====================================================================
// IPv4-заголовок и границы пакета проверены вызывающей стороной.
static void tcp_proxy_client_handle_non_tcp(struct tcp_proxy_client* p, uint8_t* buf, size_t len, uint16_t ihl) {
    uint8_t* transport = buf + ihl;
    uint8_t proto = buf[9];
    if (proto == IPPROTO_UDP) {
        size_t available = len - ihl;
        uint16_t udp_len = available >= 8 ? ((uint16_t)transport[4] << 8) | transport[5] : 0;
        if (udp_len < 8 || udp_len > available) {
            DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy TUN: invalid UDP length=%u available=%zu", udp_len, available);
            return;
        }
        uint32_t src_ip, dst_ip; uint16_t src_port, dst_port;
        memcpy(&src_ip, buf + 12, 4); memcpy(&dst_ip, buf + 16, 4);
        memcpy(&src_port, transport, 2); memcpy(&dst_port, transport + 2, 2);
        udp_proxy_send_to_exit(p->inst, p->via_ed, src_ip, src_port, dst_ip, dst_port, transport + 8, udp_len - 8);
        return;
    }
    if (proto == IPPROTO_ICMP) {
        if (len - ihl < 8) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy TUN: truncated ICMP header"); return; }
        uint8_t icmp_type = transport[0];
        if (icmp_type != 8) return;
        uint32_t src_ip, dst_ip;
        memcpy(&src_ip, buf + 12, 4); memcpy(&dst_ip, buf + 16, 4);
        uint16_t icmp_id, icmp_seq;
        memcpy(&icmp_id,  transport + 4, 2); memcpy(&icmp_seq, transport + 6, 2);
        icmp_proxy_send_to_exit(p->inst, p->via_ed, dst_ip, src_ip, icmp_id, icmp_seq, transport + 8, len - ihl - 8);
    }
}

// ====================================================================
// Помощник: передача данных из очереди to_lwip в lwIP TCP
// ====================================================================
static void tcp_proxy_client_feed_from_transport(struct tcp_proxy_client_conn *pc) {
    if (!pc->to_lwip || !pc->pcb || pc->rem_closed) return;
    int sent = 0;
    while (pc->to_lwip->head) {
        uint16_t space = tcp_sndbuf(pc->pcb);
        if (!space) break;
        struct ll_entry* e = queue_data_get(pc->to_lwip);
        uint16_t len = e->len < space ? e->len : space;
        err_t ret = tcp_write(pc->pcb, e->dgram, len, TCP_WRITE_FLAG_COPY);
        if (ret != LERR_OK) {
            DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy: lwIP write blocked sid=%08x ret=%d space=%u", pc->stream_id, ret, space);
            queue_data_put_first(pc->to_lwip, e); break;
        }
        sent = 1;
        e->len -= len;
        if (e->len) { memmove(e->dgram, e->dgram + len, e->len); queue_data_put_first(pc->to_lwip, e); }
        else { queue_dgram_free(e); queue_entry_free(e); }
        // Буфер lwIP тоже ограничен snd_buf; to_lwip ограничен окном proxy.
        proxy_flow_consume(&pc->flow, len);
    }
    queue_resume_callback(pc->to_lwip);
    if (sent) tcp_output(pc->pcb);
    if (pc->fin_deferred && !pc->to_lwip->head) {
        err_t ret = tcp_shutdown(pc->pcb, 0, 1);
        if (ret == LERR_OK) {
            pc->fin_deferred = 0;
            DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy: lwIP FIN queued sid=%08x", pc->stream_id);
        } else DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy: lwIP FIN retry sid=%08x ret=%d", pc->stream_id, ret);
    }
}

// ====================================================================
// lwIP TCP коллбэки
// ====================================================================
static void tcp_proxy_client_open_failed(void* arg, int error) {
    struct tcp_proxy_client_conn* pc = arg;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "TCP proxy CONNECT failed sid=%08x error=%d", pc->stream_id, error);
    pc->error = 1;
    pc->close_pending = pc->flow.connect_sent;
    tcp_proxy_client_abort_pcb(pc, "CONNECT");
    tcp_proxy_client_conn_free(pc);
}

static err_t tcp_proxy_client_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    struct tcp_proxy_client *p = (struct tcp_proxy_client *)arg;
    if (err != LERR_OK || !newpcb) return LERR_ABRT;
    if (!tcp_proxy_client_can_accept(p)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "TCP proxy TUN stream capacity exhausted");
        tcp_abort(newpcb); return LERR_ABRT;
    }

    struct tcp_proxy_client_conn *pc = u_calloc(1, sizeof(struct tcp_proxy_client_conn));
    if (!pc) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy client: accept alloc failed"); return LERR_MEM; }
    pc->proxy = p; pc->pcb = newpcb; pc->stream_id = ++p->next_stream_id;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "PROXY ACCEPT pcb=%p sid=%08x", (void*)newpcb, pc->stream_id);
    proxy_flow_init(&pc->flow, p->inst, p->ua, TOPO_GROUP_UTUN, p->via_ed, ETCP_RT_ID_TCP_PROXY_SERVER, pc->stream_id, tcp_proxy_client_flow_wake, pc);
    pc->flow.client = p;

    int i;
    for (i = 0; i < p->mapping_count; i++) {
        if (p->mappings[i].local_port == newpcb->local_port) {
            struct in_addr ra; ra.s_addr = inet_addr(p->mappings[i].remote_ip);
            memcpy(pc->dest_ip, &ra.s_addr, 4);
            pc->dest_port = htons(p->mappings[i].remote_port);
            break;
        }
    }
    if (i == p->mapping_count) {
        memcpy(pc->dest_ip, &newpcb->local_ip, 4);
        pc->dest_port = htons(newpcb->local_port);
    }

    tcp_arg(newpcb, pc);
    tcp_recv(newpcb, tcp_proxy_client_recv_cb);
    tcp_sent(newpcb, tcp_proxy_client_sent_cb);
    tcp_err(newpcb, tcp_proxy_client_err_cb);
    tcp_poll(newpcb, tcp_proxy_client_poll_cb, 2);
    tcp_nagle_disable(newpcb);

    pc->to_lwip = queue_new(p->ua, 0, 0, 0, "to_lwip");
    pc->tx_queue = queue_new(p->ua, 0, 0, 0, "tx_queue");
    if (!pc->tx_queue || !pc->to_lwip) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy client: tx_queue alloc failed sid=%08x", pc->stream_id);
        queue_free(pc->to_lwip); queue_free(pc->tx_queue); pc->to_lwip = NULL;
        tcp_arg(newpcb, NULL); tcp_abort(newpcb); u_free(pc); return LERR_ABRT;
    }
    queue_set_callback(pc->tx_queue, tcp_proxy_client_tx_queue_drain_cb, pc);
    // Локальный exit может прервать PCB во время DATA. Сначала lwIP должен закончить recv всего pbuf.
    if (tcp_proxy_client_is_local(p)) queue_set_callback_defer(pc->tx_queue, 1);

    pc->next = p->conns; p->conns = pc; p->conn_count++;
    int ret = tcp_proxy_client_open(&pc->flow, pc->dest_ip, pc->dest_port, tcp_proxy_client_open_failed);
    if (ret < 0) { tcp_proxy_client_open_failed(pc, -ret); return LERR_ABRT; }
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "TCP proxy client: new conn sid=%08x local_port=%u -> %d.%d.%d.%d:%d total_conns=%d snd_wnd=%u mss=%u",
               pc->stream_id, newpcb->local_port, pc->dest_ip[0], pc->dest_ip[1], pc->dest_ip[2], pc->dest_ip[3], ntohs(pc->dest_port), p->conn_count, newpcb->snd_wnd, newpcb->mss);
    return LERR_OK;
}

// ====================================================================
// Backpressure: буфер lwIP→ETCP (tx_queue) с пачковым восстановлением окна.
// Остановленные потоки возобновляет общий диспетчер отправки клиента.
// ====================================================================

// После отправки последних данных (очередь пуста): релеить FIN/CLOSE в exit.
// Возвращает 1, если соединение завершено и pc освобождён (дальше pc/q использовать нельзя).
static int tcp_proxy_client_fin_flush(struct tcp_proxy_client_conn* pc) {
    if (pc->fin_local && !pc->tx_queue->head && !pc->flow.fin_sent) {
        pc->flow.fin_pending = 1;
        proxy_flow_flush(&pc->flow);
    }
    // Локальная сторона (браузер) завершена — отсоединяем pcb сразу, не дожидаясь
    // доставки FIN в exit (иначе таймер TIME_WAIT освободит pcb без err_cb, pc->pcb
    // останется висячим и последующий tcp_abort даст double free).
    if (pc->fin_local && pc->fin_remote && !pc->fin_deferred && !pc->to_lwip->head) {
        tcp_proxy_client_conn_detach(pc);
    }
    if (pc->flow.fin_sent && pc->fin_remote && !pc->fin_deferred && !pc->tx_queue->head && !pc->to_lwip->head) {
        tcp_proxy_client_conn_free(pc);
        return 1;
    }
    return 0;
}

static void tcp_proxy_client_flow_wake(void* arg) {
    struct tcp_proxy_client_conn* pc = arg;
    if (pc->rem_closed || pc->error) return;
    if (!pc->flow.data_task.link.owner) queue_resume_callback(pc->tx_queue);
    tcp_proxy_client_feed_from_transport(pc);
    tcp_proxy_client_fin_flush(pc);
}

// Дрейн tx_queue → ETCP. tcp_recved вызывается только после успешной отправки:
// окно закрывается плавно (штатная backpressure), а при возобновлении открывается
// пачкой (за 2 пакета wnd_inflation ≥ TCP_WND_UPDATE_THRESHOLD → немедленный ACK).
static void tcp_proxy_client_tx_deferred(void* arg) {
    struct tcp_proxy_client_conn* pc = arg;
    tcp_proxy_client_tx_queue_drain_cb(pc->tx_queue, pc);
}

static void tcp_proxy_client_tx_queue_drain_cb(struct ll_queue* q, void* arg) {
    struct tcp_proxy_client_conn* pc = (struct tcp_proxy_client_conn*)arg;
    if (!pc || !pc->proxy || pc->rem_closed) return;

    struct ll_entry* e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); return; }

    struct tcp_proxy_client* proxy = pc->proxy;
    uint32_t sid = pc->stream_id;
    int local = tcp_proxy_client_is_local(proxy);
    int ret = proxy_flow_send(&pc->flow, e->dgram, e->len, 0);
    // При loopback после send нам принадлежит только снятая с очереди запись из пула клиента.
    if (local && ret == 0) {
        pc = tcp_proxy_client_find_conn(proxy, sid);
        if (!pc) {
            DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy local DATA completed after stream close sid=%08x bytes=%u", sid, e->len);
            queue_dgram_free(e); queue_entry_free(e); return;
        }
    }
    if (ret == 0) {
        pc->bytes_to_exit += e->len;
        pc->fin_wait_deadline_tb = 0;
        if (pc->pcb) tcp_recved(pc->pcb, e->len);
        queue_dgram_free(e); queue_entry_free(e);
        if (queue_entry_count(q) == 0 && tcp_proxy_client_fin_flush(pc)) return;
        if (q->head && pc->proxy->tx_running)
            tcp_proxy_client_schedule(pc->proxy, &pc->flow.data_task, PROXY_TX_DATA, tcp_proxy_client_tx_deferred, pc);
        else queue_resume_callback(q);
    } else {
        queue_data_put_first(q, e);
        pc->bp_count++;
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "PROXY BP stall sid=%08x q=%d rcv_wnd=%u ret=%d stalls=%u",
                    sid, queue_entry_count(q), pc->pcb ? pc->pcb->rcv_wnd : 0, ret, pc->bp_count);
        if (ret != -2) tcp_proxy_client_schedule(pc->proxy, &pc->flow.data_task, PROXY_TX_DATA, tcp_proxy_client_tx_deferred, pc);
    }
}

static err_t tcp_proxy_client_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct tcp_proxy_client_conn *pc = (struct tcp_proxy_client_conn *)arg;
    if (!pc || !pc->proxy || pc->rem_closed) { if (p) pbuf_free(p); return LERR_OK; }

    if (p == NULL || err != LERR_OK) {
        pc->fin_local = 1;
        if (p == NULL && err == ERR_OK) {
            DEBUG_INFO(DEBUG_CATEGORY_PROXY, "PROXY FIN sid=%08x to_exit=%u from_exit=%u bp#=%u pcb_state=%u sndbuf=%u cwnd=%u",
                       pc->stream_id, pc->bytes_to_exit, pc->bytes_from_exit, pc->bp_count,
                       pcb->state, pcb->snd_buf, pcb->cwnd);
            { uint16_t wnd_gap = TCP_WND_MAX(pcb) - pcb->rcv_wnd;
              if (wnd_gap > 0) tcp_recved(pcb, wnd_gap); }
            tcp_proxy_client_fin_flush(pc); // После встречного FIN PCB уже может перейти в TIME_WAIT при непустой tx_queue.
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "PROXY ERROR recv sid=%08x pcb_state=%u err=%d",
                        pc->stream_id, pcb->state, err);
            pc->error = 1;
            tcp_proxy_client_send_error(pc);
        }
        return LERR_OK;
    }

    uint16_t len = p->tot_len;
    if (pc->error || pc->rem_closed) {
        tcp_recved(pcb, len); pbuf_free(p); return LERR_OK;
    }

    struct ll_entry* head = NULL;
    struct ll_entry** tail = &head;
    for (uint32_t off = 0; off < len;) {
        uint16_t chunk = len - off > TCP_PROXY_CHUNK ? TCP_PROXY_CHUNK : len - off;
        struct ll_entry* e = queue_entry_new_from_pool(pc->proxy->entry_pool);
        if (e) { e->dgram = u_malloc(chunk); e->len = chunk; }
        if (!e || !e->dgram) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy: recv allocation failed sid=%08x len=%u", pc->stream_id, len);
            if (e) queue_entry_free(e);
            while (head) { struct ll_entry* next = head->next; queue_dgram_free(head); queue_entry_free(head); head = next; }
            return LERR_MEM;
        }
        pbuf_copy_partial(p, e->dgram, chunk, off);
        e->next = NULL; *tail = e; tail = &e->next; off += chunk;
    }
    if (len) pc->fin_wait_deadline_tb = 0;
    while (head) { struct ll_entry* next = head->next; head->next = NULL; queue_data_put(pc->tx_queue, head); head = next; }
    pbuf_free(p);
    return LERR_OK;
}

static err_t tcp_proxy_client_sent_cb(void *arg, struct tcp_pcb *pcb, uint16_t len) {
    (void)len;
    struct tcp_proxy_client_conn *pc = (struct tcp_proxy_client_conn *)arg;
    if (!pc || !pc->proxy || pc->rem_closed) return LERR_OK;
    tcp_proxy_client_feed_from_transport(pc);
    tcp_proxy_client_fin_flush(pc);
    return LERR_OK;
}

static void tcp_proxy_client_err_cb(void *arg, err_t err) {
    struct tcp_proxy_client_conn *pc = (struct tcp_proxy_client_conn *)arg;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "PROXY ERR_CB pc=%p pcb=%p sid=%08x err=%d rem_closed=%d",
                (void*)pc, (void*)(pc ? pc->pcb : NULL), pc ? pc->stream_id : 0, err, pc ? pc->rem_closed : 0);
    if (!pc || !pc->proxy || pc->rem_closed) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "PROXY tcp_proxy_client_err_cb: pc=%p proxy=%p rem_closed=%d err=%d", (void*)pc, pc ? (void*)pc->proxy : NULL, pc ? pc->rem_closed : 0, err); return; }
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy client: error %d sid=%08x fin_local=%d rem_closed=%d",
                 err, pc->stream_id, pc->fin_local, pc->rem_closed);
    pc->pcb = NULL; // pcb уже освобождён (или вот-вот) стеком lwip — не разыменовывать
    if (err != LERR_CLSD) {
        pc->error = 1;
        if (tcp_proxy_client_send_error(pc) < 0) DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "PROXY send_error failed sid=%08x", pc->stream_id);
    }
    tcp_proxy_client_conn_free(pc);
}

static err_t tcp_proxy_client_poll_cb(void *arg, struct tcp_pcb *pcb) {
    struct tcp_proxy_client_conn *pc = (struct tcp_proxy_client_conn *)arg;
    if (!pc || !pc->proxy || pc->rem_closed) return LERR_OK;

    if (pcb->state == FIN_WAIT_2) {
        uint64_t now = get_time_tb();
        if (!pc->fin_wait_deadline_tb) pc->fin_wait_deadline_tb = now + TCP_FIN_WAIT_TIMEOUT * 10;
        else if (now >= pc->fin_wait_deadline_tb) {
            DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy: FIN_WAIT_2 idle for %u ms sid=%08x queued=%d",
                       (unsigned)TCP_FIN_WAIT_TIMEOUT, pc->stream_id, pc->tx_queue->count);
            tcp_abort(pcb); // err_cb снимает поток и уведомляет exit.
            return LERR_ABRT;
        }
    }

    if (pc->error) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "PROXY CLEANUP error sid=%08x", pc->stream_id);
        tcp_proxy_client_abort_pcb(pc, "POLL_ABORT");
        tcp_proxy_client_conn_free(pc);
        return LERR_ABRT;
    }

    tcp_proxy_client_feed_from_transport(pc);
    if (tcp_proxy_client_fin_flush(pc)) return LERR_OK;
    if (pc->close_pending) {
        if (pc->error) tcp_proxy_client_send_error(pc);
        else tcp_proxy_client_send_close(pc);
    }
    return LERR_OK;
}

// ====================================================================
// SYN резервирует место до перехода в ESTABLISHED, RST или истечения таймера lwIP.
static int tcp_proxy_admit_syn(void* arg) {
    if (tcp_proxy_client_can_accept(arg)) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy TUN: SYN dropped at stream limit=%d", TCP_PROXY_CLIENT_STREAM_LIMIT);
    return 0;
}

// ====================================================================
// Ввод: IP пакет → lwip_tcp (из TUN output_queue)
// ====================================================================
static void tcp_proxy_client_tun_input(struct ll_queue* q, void* arg) {
    struct tcp_proxy_client* p = (struct tcp_proxy_client*)arg;
    struct ll_entry* entry = queue_data_get(q);
    if (!entry) return;
    if (entry->dgram && entry->len > 1) {
        uint8_t* ip = entry->dgram + 1; size_t len = entry->len - 1;
        uint16_t ihl = len ? (ip[0] & 15) * 4 : 0;
        uint16_t total = len >= 4 ? ((uint16_t)ip[2] << 8) | ip[3] : 0;
        if (len < 20 || (ip[0] >> 4) != 4 || ihl < 20 || total < ihl || total > len) {
            DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy TUN: invalid IPv4 packet len=%zu ihl=%u total=%u", len, ihl, total);
        } else if ((ip[6] & 0x3f) || ip[7]) {
            DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy TUN: fragmented IPv4 packet unsupported flags=%02x%02x", ip[6], ip[7]);
        } else if (ip[9] == IPPROTO_TCP) {
            uint16_t tcp_len = total - ihl;
            struct pbuf* pb = pbuf_alloc(PBUF_RAW, tcp_len);
            if (pb) {
                uint32_t src_ip, dst_ip;
                memcpy(&src_ip, ip + 12, 4); memcpy(&dst_ip, ip + 16, 4);
                pbuf_take(pb, ip + ihl, tcp_len); lwip_tcp_input(p->lwip, pb, src_ip, dst_ip);
            } else DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy TUN: TCP buffer allocation failed len=%u", tcp_len);
        } else {
            tcp_proxy_client_handle_non_tcp(p, ip, total, ihl);
        }
    }
    queue_dgram_free(entry); queue_entry_free(entry); queue_resume_callback(q);
}

// ====================================================================
// Очистка tcp_proxy_client_conn
// ====================================================================
static void tcp_proxy_client_conn_free(struct tcp_proxy_client_conn *pc) {
    if (!pc) return;
    struct tcp_proxy_client *p = pc->proxy;
    proxy_flow_destroy(&pc->flow);
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "PROXY FREE pcb=%p sid=%08x", (void*)pc->pcb, pc->stream_id);
    uint32_t pending = pc->to_lwip ? queue_entry_count(pc->to_lwip) : 0;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "PROXY FREE sid=%08x total_conns=%d to_lwip_q=%u",
               pc->stream_id, p->conn_count, pending);
    if (pc->close_pending && pc->flow.connect_sent && !pc->rem_closed && p->inst) {
        pc->close_pending = 0;
        uint8_t subcmd = pc->error ? TCP_PROXY_SUBCMD_ERROR : TCP_PROXY_SUBCMD_CLOSE;
        tcp_proxy_client_retire(p, pc->stream_id, subcmd);
    }
    struct tcp_proxy_client_conn **prev = &p->conns;
    while (*prev) { if (*prev == pc) { *prev = pc->next; p->conn_count--; break; } prev = &(*prev)->next; }
    if (pc->to_lwip) {
        struct ll_entry *e;
        while ((e = queue_data_get(pc->to_lwip))) { queue_dgram_free(e); queue_entry_free(e); }
        queue_free(pc->to_lwip); pc->to_lwip = NULL;
    }
    if (pc->tx_queue) {
        struct ll_entry *e;
        while ((e = queue_data_get(pc->tx_queue))) { queue_dgram_free(e); queue_entry_free(e); }
        queue_free(pc->tx_queue); pc->tx_queue = NULL;
    }
    u_free(pc);
}

// Отвязать pcb от conn: сбросить коллбэки и pc->pcb. lwIP дальше сам освободит pcb
// (в т.ч. молча по истечении TIME_WAIT). Контракт: после вызова pc->pcb == NULL.
static void tcp_proxy_client_conn_detach(struct tcp_proxy_client_conn *pc) {
    if (!pc || !pc->pcb) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "PROXY DETACH pcb=%p state=%s sid=%08x",
                (void*)pc->pcb, tcp_debug_state_str(pc->pcb->state), pc->stream_id);
    tcp_arg(pc->pcb, NULL);
    tcp_recv(pc->pcb, NULL);
    tcp_sent(pc->pcb, NULL);
    tcp_err(pc->pcb, NULL);
    tcp_poll(pc->pcb, NULL, 0);
    pc->pcb = NULL;
}

// Проверка, что pc->pcb ещё не освобождён пулом (защита от висячего указателя:
// таймер TIME_WAIT мог освободить pcb без err_cb). Возвращает 0 и сбрасывает
// pc->pcb, если pcb освобождён; иначе 1 (pcb валиден).
static int tcp_proxy_client_pcb_guard(struct tcp_proxy_client_conn *pc, const char* why) {
    struct tcp_pcb* pcb = pc->pcb;
    struct lwip_tcp_ctx* ctx = pc->proxy ? pc->proxy->lwip : NULL;
    if (ctx && memory_pool_is_freed(ctx->pcb_pool, pcb)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "PROXY %s: pcb=%p already freed (sid=%08x) — skip",
                   why, (void*)pcb, pc->stream_id);
        pc->pcb = NULL;
        return 0;
    }
    return 1;
}

// Безопасный abort с защитой от висячего pc->pcb (pcb уже освобождён пулом, например
// таймером TIME_WAIT). Если pcb освобождён — только сбрасываем ссылку, не трогая память.
static void tcp_proxy_client_abort_pcb(struct tcp_proxy_client_conn *pc, const char* why) {
    if (!pc || !pc->pcb) return;
    struct tcp_pcb* pcb = pc->pcb;
    if (!tcp_proxy_client_pcb_guard(pc, why)) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "PROXY %s pcb=%p state=%s sid=%08x",
                why, (void*)pcb, tcp_debug_state_str(pcb->state), pc->stream_id);
    tcp_proxy_client_conn_detach(pc);
    tcp_abort(pcb);
}

// ====================================================================
// Обработчики входящих сообщений (сторона клиента)
// ====================================================================
static struct tcp_proxy_client_conn* tcp_proxy_client_find_conn(struct tcp_proxy_client* p, uint32_t stream_id) {
    struct tcp_proxy_client_conn* pc;
    for (pc = p->conns; pc; pc = pc->next) if (pc->stream_id == stream_id) return pc;
    return NULL;
}

static void tcp_proxy_client_handle_data(struct tcp_proxy_client* p, struct ETCP_CONN* conn, uint32_t stream_id, struct ll_entry* entry) {
    struct tcp_proxy_client_conn* pc = tcp_proxy_client_find_conn(p, stream_id);
    if (!pc || pc->rem_closed || pc->error) {
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "PROXY DATA sid=%08x — no conn/closed, dropping", stream_id);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    size_t data_len = entry->len - TCP_PROXY_RECV_HDR_SIZE;
    DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "PROXY DATA <- sid=%08x len=%zu", stream_id, data_len);
    if (data_len > 0) {
        pc->bytes_from_exit += (uint32_t)data_len;
        struct ll_entry* e = tcp_proxy_client_entry_from_data(pc->proxy->entry_pool, entry->dgram + TCP_PROXY_RECV_HDR_SIZE, (uint16_t)data_len);
        if (!e || proxy_flow_receive(&pc->flow, data_len) < 0) {
            if (e) { queue_dgram_free(e); queue_entry_free(e); }
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy: receive failed sid=%08x", stream_id);
            tcp_proxy_client_send_error(pc);
            tcp_proxy_client_abort_pcb(pc, "DATA_ABORT");
            tcp_proxy_client_conn_free(pc);
        } else {
            queue_data_put(pc->to_lwip, e);
            tcp_proxy_client_feed_from_transport(pc);
        }
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void tcp_proxy_client_handle_close(struct tcp_proxy_client* p, uint32_t stream_id) {
    struct tcp_proxy_client_conn* pc = tcp_proxy_client_find_conn(p, stream_id);
    if (!pc) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "PROXY CLOSE sid=%08x — no conn, dropping", stream_id); return; }
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "PROXY REM_CLOSED sid=%08x fin_local=%d pcb=%p", stream_id, pc->fin_local, (void*)pc->pcb);
    pc->rem_closed = 1;
    if (pc->pcb && tcp_proxy_client_pcb_guard(pc, "CLOSE")) {
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "PROXY CLOSE pcb=%p state=%s sid=%08x",
                    (void*)pc->pcb, tcp_debug_state_str(pc->pcb->state), stream_id);
        tcp_arg(pc->pcb, NULL);
        tcp_recv(pc->pcb, NULL);
        tcp_sent(pc->pcb, NULL);
        tcp_err(pc->pcb, NULL);
        tcp_poll(pc->pcb, NULL, 0);
        tcp_close(pc->pcb);
        pc->pcb = NULL;
    }
    tcp_proxy_client_conn_free(pc);
}

static void tcp_proxy_client_handle_error(struct tcp_proxy_client* p, uint32_t stream_id) {
    struct tcp_proxy_client_conn* pc = tcp_proxy_client_find_conn(p, stream_id);
    if (!pc) { DEBUG_INFO(DEBUG_CATEGORY_PROXY, "PROXY ERROR sid=%08x — no conn, dropping", stream_id); return; }
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "PROXY ERROR from exit sid=%08x fin_local=%d", stream_id, pc->fin_local);
    tcp_proxy_client_abort_pcb(pc, "ERROR");
    pc->rem_closed = 1;
    tcp_proxy_client_conn_free(pc);
}

static void tcp_proxy_client_handle_fin(struct tcp_proxy_client* p, uint32_t stream_id) {
    struct tcp_proxy_client_conn* pc = tcp_proxy_client_find_conn(p, stream_id);
    if (!pc || !pc->pcb) return;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "PROXY FIN FROM exit sid=%08x pcb_state=%u to_lwip=%d — shutdown write (send FIN to local)",
               stream_id, pc->pcb->state, pc->to_lwip ? queue_entry_count(pc->to_lwip) : -1);
    if (pc->flow.fin_received) return;
    pc->fin_remote = 1; pc->flow.fin_received = 1; pc->fin_deferred = 1;
    tcp_proxy_client_feed_from_transport(pc);
    tcp_proxy_client_fin_flush(pc);
}

// ====================================================================
// ETCP коллбэк клиентской стороны (принимает DATA/CLOSE/ERROR/FIN от сервера)
// ====================================================================
// Все локальные потоки привязаны к одному exit. После закрытия исходящего канала не создаём новый ради CLOSE/ERROR.
static void tcp_proxy_client_close_streams(struct tcp_proxy_client* proxy, int notify_exit) {
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "PROXY CLOSE_ALL peer=%s lwip=%d socks=%d http=%d notify_exit=%d",
               sc_public_key_hex(proxy->via_ed, (char[65]){0}), proxy->conn_count, proxy->socks_conn_count, proxy->http_conn_count, notify_exit);
    while (proxy->conns) {
        struct tcp_proxy_client_conn* pc = proxy->conns;
        if (!notify_exit) pc->close_pending = 0;
        tcp_proxy_client_abort_pcb(pc, "CLOSE_ALL");
        tcp_proxy_client_conn_free(pc);
    }
    socks_proxy_conn_free_all(&proxy->socks_conns, notify_exit);
    socks_proxy_conn_free_all(&proxy->http_conns, notify_exit);
    if (!notify_exit) {
        proxy->terminal_count = 0;
        if (proxy->tx_retry) uasync_timer_set(proxy->tx_retry, -1);
        uasync_cancel_work(&proxy->tx_kick);
        if (proxy->tx_waiter.internal || proxy->tx_waiter.call_soon_id) queue_waiter_cancel(proxy->tx_waiter.defer_q, &proxy->tx_waiter);
    }
}

// Исходящий router-канал имеет service ID сервера, поэтому подписка клиента отдельная от приёма.
static void tcp_proxy_client_tx_closed(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]) {
    struct tcp_proxy_client* proxy = inst->tcp_proxy_client;
    if (proxy && group_id == TOPO_GROUP_UTUN && !memcmp(ed, proxy->via_ed, 32)) tcp_proxy_client_close_streams(proxy, 0);
}

static const struct router_tx_policy tcp_proxy_client_tx_policy = {
    .ack_timeout_tb = 300000, // 30 секунд без нового ACK при непустой очереди
    .closed = tcp_proxy_client_tx_closed,
};

void tcp_proxy_client_router_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    struct tcp_proxy_client* proxy = inst ? inst->tcp_proxy_client : NULL;

    if (!entry || !entry->dgram || entry->len < ROUTER_SVC_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy client: invalid router header length=%u", entry ? entry->len : 0);
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }
    uint64_t group;
    const uint8_t* peer = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    if (!proxy || entry->dgram[0] != ETCP_RT_ID_TCP_PROXY_CLIENT || memcmp(peer, proxy->via_ed, 32) || group != TOPO_GROUP_UTUN) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy client: unexpected peer=%s group=%016llx",
                   sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)group);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    // CLOSE_ALL / restart-уведомление: [svc_id][src][dst] без payload
    if (entry->len == ROUTER_SVC_HDR_SIZE) {
        tcp_proxy_client_close_streams(proxy, 1);
        queue_dgram_free(entry); queue_entry_free(entry);
        return;
    }
    if (entry->len < TCP_PROXY_RECV_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy client: incomplete command header len=%u", entry->len);
        queue_dgram_free(entry); queue_entry_free(entry);
        return;
    }
    uint8_t   subcmd    = entry->dgram[ROUTER_SVC_PAYLOAD_OFF];
    uint32_t  stream_id; memcpy(&stream_id, entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, 4);

    if (!proxy_command_valid(ETCP_RT_ID_TCP_PROXY_CLIENT, subcmd, entry->len - TCP_PROXY_RECV_HDR_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy client: invalid cmd=%u sid=%08x length=%u", subcmd, stream_id, entry->len);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "PROXY CLIENT RECV subcmd=%02x sid=%08x pkt_len=%u", subcmd, stream_id, entry->len);

    if (proxy) {
        size_t data_len = entry->len > TCP_PROXY_RECV_HDR_SIZE ? entry->len - TCP_PROXY_RECV_HDR_SIZE : 0;
        // Пробуем SOCKS/HTTP first (у них приоритет — могут быть без TUN)
        if (proxy->socks_enabled && socks_proxy_handle_etcp(&proxy->socks_conns, &proxy->socks_conn_count,
                stream_id, subcmd, entry->dgram + TCP_PROXY_RECV_HDR_SIZE, data_len)) {
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        if (proxy->http_proxy_enabled && socks_proxy_handle_etcp(&proxy->http_conns, &proxy->http_conn_count,
                stream_id, subcmd, entry->dgram + TCP_PROXY_RECV_HDR_SIZE, data_len)) {
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        struct tcp_proxy_client_conn* pc = tcp_proxy_client_find_conn(proxy, stream_id);
        if (pc && proxy_client_early_frame(&pc->flow, subcmd)) {
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        if (pc && subcmd == TCP_PROXY_SUBCMD_CONNECTED) {
            if (pc->flow.opening && pc->flow.connect_sent) {
                tcp_proxy_client_cancel_open(&pc->flow);
                pc->flow.ready = 1;
                tcp_proxy_client_flow_wake(pc);
            } else DEBUG_WARN(DEBUG_CATEGORY_PROXY, "TCP proxy unexpected CONNECTED sid=%08x", stream_id);
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        if (pc && subcmd == TCP_PROXY_SUBCMD_WINDOW) {
            if (proxy_flow_window(&pc->flow, entry->dgram + TCP_PROXY_RECV_HDR_SIZE, data_len) < 0)
                tcp_proxy_client_handle_error(proxy, stream_id);
            else tcp_proxy_client_flow_wake(pc);
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        // Существующие lwIP conns
        if (subcmd == TCP_PROXY_SUBCMD_DATA)  { tcp_proxy_client_handle_data(proxy, conn, stream_id, entry); return; }
        if (subcmd == TCP_PROXY_SUBCMD_CLOSE) { tcp_proxy_client_handle_close(proxy, stream_id); queue_dgram_free(entry); queue_entry_free(entry); return; }
        if (subcmd == TCP_PROXY_SUBCMD_ERROR) { tcp_proxy_client_handle_error(proxy, stream_id); queue_dgram_free(entry); queue_entry_free(entry); return; }
        if (subcmd == TCP_PROXY_SUBCMD_FIN)   { tcp_proxy_client_handle_fin(proxy, stream_id); queue_dgram_free(entry); queue_entry_free(entry); return; }
    }
    if (subcmd == TCP_PROXY_SUBCMD_DATA || subcmd == TCP_PROXY_SUBCMD_CLOSE || subcmd == TCP_PROXY_SUBCMD_FIN ||
        subcmd == TCP_PROXY_SUBCMD_ERROR || subcmd == TCP_PROXY_SUBCMD_WINDOW) {
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "PROXY subcmd=%02x sid=%08x — client conn not found, silent drop", subcmd, stream_id);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "TCP proxy client: unhandled subcmd=%02x sid=%08x", subcmd, stream_id);
    queue_dgram_free(entry); queue_entry_free(entry);
}

// ====================================================================
// Публичное API
// ====================================================================
struct tcp_proxy_client* tcp_proxy_client_create(struct UTUN_INSTANCE* inst, struct UASYNC* ua,
    const char* tun_name, const char* tun_ip, int mtu, int test_mode,
    struct tcp_proxy_client_mapping_config* mappings, int mapping_count,
    const uint8_t via_ed[32],
    int socks_enabled, const char* socks_addr,
    int http_proxy_enabled, const char* http_proxy_addr)
{
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "ua is NULL"); return NULL; }
    if (inst && (inst->tcp_proxy_client || inst->router_bindings.callbacks[ETCP_RT_ID_TCP_PROXY_CLIENT])) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy client: duplicate instance binding"); return NULL;
    }
    if ((socks_enabled && (!socks_addr || !socks_addr[0])) ||
        (http_proxy_enabled && (!http_proxy_addr || !http_proxy_addr[0])) || mapping_count < 0 || (mapping_count && !mappings)) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy client: missing listener address or invalid mappings"); return NULL;
    }

    int need_tun = tun_name && tun_name[0] && tun_ip && tun_ip[0];
    if ((tun_name && tun_name[0]) != (tun_ip && tun_ip[0])) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy client: TUN requires both name and IP"); return NULL;
    }
    if (!need_tun && !socks_enabled && !http_proxy_enabled) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "no proxy mode enabled"); return NULL;
    }

    uint8_t x[32];
    if ((inst || via_ed) && sc_identity_public_x("proxy exit", via_ed, x) != SC_OK) return NULL;
    struct tcp_proxy_client* p = u_calloc(1, sizeof(struct tcp_proxy_client));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "u_calloc failed"); return NULL; }
    p->inst = inst; p->ua = ua; p->next_stream_id = 1;
    if (via_ed) memcpy(p->via_ed, via_ed, 32);
    p->mappings = mappings; p->mapping_count = mapping_count;
    p->socks_enabled = socks_enabled;
    p->http_proxy_enabled = http_proxy_enabled;

    p->entry_pool = memory_pool_init(sizeof(struct ll_entry), "entry_pool");
    if (!p->entry_pool) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "memory_pool_init failed"); u_free(p); return NULL; }

    if (need_tun) {
        p->tun = tun_init_nat(ua, tun_name, tun_ip, mtu > 0 ? mtu : 1500, test_mode);
        if (!p->tun) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "tcp_proxy_client: failed to create TUN %s", tun_name); memory_pool_destroy(p->entry_pool); u_free(p); return NULL; }
        queue_set_callback(p->tun->output_queue, tcp_proxy_client_tun_input, p);

        p->lwip = lwip_tcp_init(ua, tcp_proxy_client_output_cb, p);
        if (!p->lwip) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "lwip_tcp_init failed"); tun_close(p->tun); memory_pool_destroy(p->entry_pool); u_free(p); return NULL; }

        if (lwip_tcp_set_mtu(p->lwip, p->tun->mtu) != LERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy MTU setup failed: mtu=%d", p->tun->mtu);
            tcp_proxy_client_destroy(p); return NULL;
        }

        for (int i = 0; i < mapping_count; i++) {
            for (int j = 0; j < i; j++) {
                if (mappings[i].local_port == mappings[j].local_port) {
                    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy TUN: duplicate mapping port=%u", mappings[i].local_port);
                    tcp_proxy_client_destroy(p); return NULL;
                }
            }
        }
        struct tcp_pcb* pcb = tcp_new(p->lwip);
        struct tcp_pcb* listener = pcb ? tcp_listen(pcb) : NULL;
        if (!listener) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy TUN: cannot allocate transparent listener");
            if (pcb) tcp_abort(pcb);
            tcp_proxy_client_destroy(p); return NULL;
        }
        tcp_arg(listener, p); tcp_accept(listener, tcp_proxy_client_accept_cb);
        ((struct tcp_pcb_listen*)listener)->admit = tcp_proxy_admit_syn;
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "proxy TUN: transparent listener ready mappings=%d", mapping_count);
    }

    for (int kind = 0; kind < PROXY_TX_TASKS; kind++) {
        p->tx_tasks[kind] = queue_new(ua, 0, 0, 0, "proxy TX tasks");
        if (!p->tx_tasks[kind]) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy TX task queue allocation failed kind=%d", kind);
            tcp_proxy_client_destroy(p); return NULL;
        }
    }
    p->tx_retry = uasync_reserve_timer(ua, p, tcp_proxy_client_tx_retry, "proxy_tx_retry");
    if (!p->tx_retry) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy TX retry timer reservation failed");
        tcp_proxy_client_destroy(p); return NULL;
    }
    p->next_open = 1;

    if (socks_enabled && socks_addr && socks_addr[0]) {
        p->socks_listen = socks_proxy_init_listen(ua, socks_addr, &p->socks_conns, &p->socks_conn_count,
                                                   &p->next_stream_id, p, 0);
        if (!p->socks_listen) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "tcp_proxy_client: SOCKS init failed on %s", socks_addr);
            tcp_proxy_client_destroy(p); return NULL;
        }
    }

    if (http_proxy_enabled && http_proxy_addr && http_proxy_addr[0]) {
        p->http_listen = socks_proxy_init_listen(ua, http_proxy_addr, &p->http_conns, &p->http_conn_count,
                                                  &p->next_stream_id, p, 1);
        if (!p->http_listen) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "tcp_proxy_client: HTTP proxy init failed on %s", http_proxy_addr);
            tcp_proxy_client_destroy(p); return NULL;
        }
    }

    if (inst) {
        if (proxy_bind_service(inst, ETCP_RT_ID_TCP_PROXY_CLIENT, tcp_proxy_client_router_recv_cb) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy client: etcp_router_bind failed");
            tcp_proxy_client_destroy(p); return NULL;
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_PROXY, "TCP proxy client: etcp_router bind registered for ID=0x%02x", ETCP_RT_ID_TCP_PROXY_CLIENT);
            p->router_bound = 1;
        }
        if (etcp_router_set_tx_policy(inst, ETCP_RT_ID_TCP_PROXY_SERVER, &tcp_proxy_client_tx_policy) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy client: cannot register live-route and ACK timeout policy");
            tcp_proxy_client_destroy(p); return NULL;
        }
    }

    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "TCP proxy client created: tun=%s socks=%s(http=%s) mappings=%d via=%s",
               need_tun ? "yes" : "no", socks_enabled ? "yes" : "no", http_proxy_enabled ? "yes" : "no",
               mapping_count, sc_public_key_hex(p->via_ed, (char[65]){0}));
    return p;
}

void tcp_proxy_client_destroy(struct tcp_proxy_client* p) {
    if (!p) return;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "TCP proxy client destroying: lwip=%d socks=%d http=%d",
               p->conn_count, p->socks_conn_count, p->http_conn_count);
    if (p->router_bound) {
        etcp_router_unbind(p->inst, ETCP_RT_ID_TCP_PROXY_CLIENT);
        etcp_router_conn_close(tcp_proxy_client_route(p));
        etcp_router_set_tx_policy(p->inst, ETCP_RT_ID_TCP_PROXY_SERVER, NULL);
        p->router_bound = 0;
    }

    if (p->socks_listen) socks_proxy_close_listen(p->ua, p->socks_listen, NULL);
    if (p->http_listen) socks_proxy_close_listen(p->ua, p->http_listen, NULL);
    tcp_proxy_client_close_streams(p, 0);
    if (p->tx_retry) uasync_cancel_timeout(p->ua, p->tx_retry);
    for (int kind = 0; kind < PROXY_TX_TASKS; kind++) if (p->tx_tasks[kind]) queue_free(p->tx_tasks[kind]);

    if (p->lwip) { lwip_tcp_destroy(p->lwip); p->lwip = NULL; }

    if (p->tun) tun_close(p->tun);
    if (p->entry_pool) memory_pool_destroy(p->entry_pool);
    u_free(p);
}
