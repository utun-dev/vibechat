/* Общий TCP proxy протокол и inline-машина окна; все вызовы выполняются в uasync владельца.
 * init задаёт окно, но ready устанавливает владелец после CONNECTED (на exit — flush).
 * Перед destroy владельца обязателен proxy_flow_destroy: retry хранит указатель на flow.
 * Отправка: svc(1), cmd(1), sid(4), payload; приём: router service header, cmd(1), sid(4), payload.
 * sid и WINDOW uint32_t копируются в host byte order; IPv4/порт CONNECT — network byte order.
 * CONNECTED подтверждает реальный connect exit → destination. DATA ограничен окном потока.
 * FIN завершает одно направление; владелец ставит fin_pending только после всех своих DATA.
 * CLOSE/ERROR и CONNECT отправляет владелец напрямую: общий retry их не сохраняет. */
#ifndef PROXY_PROTOCOL_H
#define PROXY_PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/topo_node.h"
#include "../../lib/u_async.h"
#include "../../lib/ll_queue.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"

#define TCP_PROXY_SUBCMD_CONNECT   0x01
#define TCP_PROXY_SUBCMD_CONNECTED 0x02
#define TCP_PROXY_SUBCMD_DATA      0x03
#define TCP_PROXY_SUBCMD_CLOSE     0x04
#define TCP_PROXY_SUBCMD_ERROR     0x05
#define TCP_PROXY_SUBCMD_FIN       0x06
#define TCP_PROXY_SUBCMD_WINDOW    0x07
#define TCP_PROXY_HDR_SIZE         6
#define TCP_PROXY_CONNECT_HDR_SIZE 12
#define TCP_PROXY_RECV_HDR_SIZE    (ROUTER_SVC_PAYLOAD_OFF + 5) // 31 байт с метаданными router, не wire header
#define TCP_PROXY_CHUNK            4096
#define TCP_PROXY_WINDOW           65536u

#define PROXY_SEND_BUSY (-3) // локальный порог send_q клиента; повтор по освобождению места

/* Проверить форму команды до поиска потока. svc задаёт направление CONNECT/CONNECTED. */
static inline int proxy_command_valid(uint8_t svc, uint8_t cmd, size_t len) {
    switch (cmd) {
    case TCP_PROXY_SUBCMD_CONNECT:   return svc == ETCP_RT_ID_TCP_PROXY_SERVER && len == 6;
    case TCP_PROXY_SUBCMD_CONNECTED: return svc == ETCP_RT_ID_TCP_PROXY_CLIENT && len == 0;
    case TCP_PROXY_SUBCMD_WINDOW:    return len == sizeof(uint32_t);
    case TCP_PROXY_SUBCMD_DATA:      return len > 0 && len <= TCP_PROXY_CHUNK;
    case TCP_PROXY_SUBCMD_CLOSE:
    case TCP_PROXY_SUBCMD_ERROR:
    case TCP_PROXY_SUBCMD_FIN:       return len == 0;
    default:                         return 0;
    }
}

struct tcp_proxy_client;
enum { PROXY_TX_CONTROL, PROXY_TX_CONNECT, PROXY_TX_DATA, PROXY_TX_TASKS };

/* Встроенная запись отложенной отправки; владелец отменяет её до free/reset. */
struct proxy_tx_task {
    struct ll_entry link;
    void (*run)(void*);
    void* arg;
};

struct proxy_flow {
    struct UTUN_INSTANCE* inst; // Заимствованный instance для отправки через router.
    struct UASYNC* ua;          // Заимствованный event loop; живёт дольше flow и retry.
    uint64_t group;            // Группа входящего CONNECT на exit, UTUN на клиенте.
    uint8_t peer[32];          // Ed25519 противоположной стороны потока.
    uint32_t sid;              // ID потока; на exit уникален только вместе с group и peer.
    uint8_t svc;               // Сервис получателя: TCP_PROXY_SERVER или TCP_PROXY_CLIENT.
    uint8_t ready;             // Разрешены DATA и FIN; начальное значение 0.
    uint8_t connected_pending; // Exit должен поставить CONNECTED в router перед остальными control.
    uint8_t fin_pending;       // Владелец уже передал все DATA и просит отправить FIN.
    uint8_t fin_sent;          // FIN принят router; это не ACK конечного TCP.
    uint8_t fin_received;      // Владелец принял FIN; последующие DATA отклоняются.
    uint32_t tx_credit;        // Сколько DATA ещё разрешено отправить, изначально TCP_PROXY_WINDOW.
    uint32_t rx_credit;        // Сколько DATA разрешено принять до следующего WINDOW.
    uint32_t consumed;         // Потреблённые байты, кредит которых ещё не передан router.
    void* retry;              // Отменяемый таймер control, 5000 tb = 500 мс.
    void (*wake)(void* arg);  // Возобновление очередей владельца после timer flush; может освободить владельца.
    void* arg;               // Заимствованный аргумент wake.
    struct tcp_proxy_client* client; // владелец клиентского потока; NULL на exit
    struct proxy_tx_task open_task, data_task, control_task;
    void* open_timer;        // дедлайн ожидания места или CONNECTED
    void (*open_failed)(void*, int); // локальное завершение с errno, может освободить flow
    uint8_t target[6];       // IPv4 и порт CONNECT в сетевом порядке
    uint8_t opening;         // занято место в бюджете незавершённых CONNECT
    uint8_t connect_sent;    // CONNECT принят; при локальной отмене уведомить exit
};

/* Только uasync. Проверка выполняется до копирования сообщения. */
/* Заимствовать точный адрес настроенного exit для исходящей команды; NULL без соответствующего клиента. */
int tcp_proxy_client_send_check(struct UTUN_INSTANCE* inst, const uint8_t peer[32], uint8_t cmd);
void tcp_proxy_client_schedule(struct tcp_proxy_client* client, struct proxy_tx_task* task, int kind,
                               void (*run)(void*), void* arg);
void tcp_proxy_client_cancel_open(struct proxy_flow* flow);
void tcp_proxy_client_cancel_flow(struct proxy_flow* flow);

// Разрешён ли данному пиру TCP/UDP/ICMP exit в группе с текущим составом участников.
int proxy_exit_group_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]);

// Копирует заимствованный data (len<=4096; при len>0 не NULL), отправляет в группу потока с mode=0.
// 0 = принято router, <0 = ошибка; force обходит лимит send_q, не гарантирует доставку.
// Созданный entry целиком передаётся router, который освобождает его при любом результате.
static inline int proxy_send_packet(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint8_t svc, uint8_t cmd,
                                    uint32_t sid, const uint8_t* data, size_t len, int force) {
    if (group != TOPO_GROUP_UTUN && !proxy_exit_group_allowed(inst, group, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy: send denied group=%016llx peer=%s", (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}));
        return -1;
    }
    if (len > TCP_PROXY_CHUNK) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy: oversized message sid=%08x cmd=%u len=%zu", sid, cmd, len);
        return -1;
    }
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy: entry allocation failed sid=%08x", sid); return -1; }
    e->dgram = (uint8_t*)u_malloc(TCP_PROXY_HDR_SIZE + len);
    if (!e->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy: payload allocation failed sid=%08x", sid);
        queue_entry_free(e); return -1;
    }
    e->dgram[0] = svc; e->dgram[1] = cmd;
    memcpy(e->dgram + 2, &sid, 4);
    if (len) memcpy(e->dgram + TCP_PROXY_HDR_SIZE, data, len);
    e->len = TCP_PROXY_HDR_SIZE + len;
    return etcp_route_send_key(inst, group, peer, e, force, 0);
}

static inline int proxy_send(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint8_t svc, uint8_t cmd,
                             uint32_t sid, const uint8_t* data, size_t len, int force) {
    if (svc == ETCP_RT_ID_TCP_PROXY_SERVER) {
        int status = tcp_proxy_client_send_check(inst, peer, cmd);
        if (status) return status;
        force = cmd != TCP_PROXY_SUBCMD_CONNECT && cmd != TCP_PROXY_SUBCMD_DATA;
    }
    return proxy_send_packet(inst, group, peer, svc, cmd, sid, data, len, force);
}

// Отправляет pending CONNECTED → WINDOW → FIN с force=1; неудачу повторяет через 500 мс.
// Владелец сам дренирует DATA перед fin_pending; flush не проверяет его очереди.
static inline void proxy_flow_flush(struct proxy_flow* f);

// Повторяем только недоставленные control-сообщения; DATA остаётся у владельца потока.
static inline void proxy_flow_retry(void* arg) {
    struct proxy_flow* f = (struct proxy_flow*)arg;
    f->retry = NULL;
    proxy_flow_flush(f);
    if (f->wake) f->wake(f->arg);
}

// Обнуляет неактивный f, задаёт peer/svc/sid и оба окна по 64 KiB; указатели заимствуются.
// Перед повторным init нужно отменить прежний retry через destroy; ready остаётся 0.
static inline void proxy_flow_init(struct proxy_flow* f, struct UTUN_INSTANCE* inst, struct UASYNC* ua,
                                  uint64_t group, const uint8_t peer[32], uint8_t svc, uint32_t sid, void (*wake)(void*), void* arg) {
    memset(f, 0, sizeof(*f));
    f->inst = inst; f->ua = ua; f->group = group; memcpy(f->peer, peer, 32); f->svc = svc; f->sid = sid;
    f->tx_credit = TCP_PROXY_WINDOW; f->rx_credit = TCP_PROXY_WINDOW;
    f->wake = wake; f->arg = arg;
}

// Отменяет только control retry; память flow, очереди и router waiter освобождает владелец.
static inline void proxy_flow_destroy(struct proxy_flow* f) {
    if (f->retry) { uasync_cancel_timeout(f->ua, f->retry); f->retry = NULL; }
    if (f->client) tcp_proxy_client_cancel_flow(f);
}

// Сначала CONNECTED, затем WINDOW и FIN. Force обходит лишь очередь router, не окно потока.
static inline void proxy_flow_flush(struct proxy_flow* f) {
    if (f->connected_pending) {
        if (proxy_send(f->inst, f->group, f->peer, f->svc, TCP_PROXY_SUBCMD_CONNECTED, f->sid, NULL, 0, 1) < 0) goto retry;
        f->connected_pending = 0; f->ready = 1;
    }
    if (f->consumed) {
        uint32_t credit = f->consumed;
        // Резервируем до синхронной loopback-доставки.
        f->consumed = 0; f->rx_credit += credit;
        if (proxy_send(f->inst, f->group, f->peer, f->svc, TCP_PROXY_SUBCMD_WINDOW, f->sid, (uint8_t*)&credit, 4, 1) < 0) {
            f->consumed += credit; f->rx_credit -= credit; goto retry;
        }
    }
    if (f->fin_pending && f->ready) {
        if (proxy_send(f->inst, f->group, f->peer, f->svc, TCP_PROXY_SUBCMD_FIN, f->sid, NULL, 0, 1) < 0) goto retry;
        f->fin_pending = 0; f->fin_sent = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy: FIN queued peer=%s sid=%08x", sc_public_key_hex(f->peer, (char[65]){0}), f->sid);
    }
    return;
retry:
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy: control retry sid=%08x connected=%u credit=%u fin=%u",
                f->sid, f->connected_pending, f->consumed, f->fin_pending);
    if (f->client) tcp_proxy_client_schedule(f->client, &f->control_task, PROXY_TX_CONTROL, proxy_flow_retry, f);
    else if (!f->retry) f->retry = uasync_set_timeout(f->ua, 5000, f, proxy_flow_retry, "proxy_control");
}

// Копирует DATA; 0 = принято router, -2 = ждём окно/CONNECTED, -1 = неверный DATA/ошибка router или выделения.
// При ошибке восстанавливает tx_credit; исходный буфер остаётся у владельца для повтора.
static inline int proxy_flow_send(struct proxy_flow* f, const uint8_t* data, size_t len, int force) {
    if (!f->ready || len > f->tx_credit) return -2;
    if (!len || len > TCP_PROXY_CHUNK || f->fin_sent) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy: invalid DATA sid=%08x len=%zu fin=%u", f->sid, len, f->fin_sent);
        return -1;
    }
    f->tx_credit -= len;
    int ret = proxy_send(f->inst, f->group, f->peer, f->svc, TCP_PROXY_SUBCMD_DATA, f->sid, data, len, force);
    if (ret < 0) f->tx_credit += len;
    return ret;
}

// Клиент принимает DATA и FIN только после CONNECTED. Возвращает 1, если команду следует пропустить.
static inline int proxy_client_early_frame(const struct proxy_flow* f, uint8_t cmd) {
    if (f->ready || (cmd != TCP_PROXY_SUBCMD_DATA && cmd != TCP_PROXY_SUBCMD_FIN)) return 0;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy client: premature frame sid=%08x cmd=%u opening=%u", f->sid, cmd, f->opening);
    return 1;
}

// Проверяет DATA len=1..4096, доступное rx_credit и отсутствие FIN; 0 списывает кредит, -1 = ошибка.
// Проверку ready и доставку байтов локальному TCP выполняет вызывающая сторона.
static inline int proxy_flow_receive(struct proxy_flow* f, size_t len) {
    if (!len || len > TCP_PROXY_CHUNK || len > f->rx_credit || f->fin_received) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy: invalid receive sid=%08x len=%zu credit=%u fin=%u",
                    f->sid, len, f->rx_credit, f->fin_received);
        return -1;
    }
    f->rx_credit -= len;
    return 0;
}

// WINDOW: заимствованные 4 байта host-order credit; 0 добавляет кредит, -1 = неверный размер/кредит.
// Не вызывает wake: владелец после успеха сам возобновляет отправку.
static inline int proxy_flow_window(struct proxy_flow* f, const uint8_t* data, size_t len) {
    uint32_t credit = 0;
    if (len == 4) memcpy(&credit, data, 4);
    if (!credit || credit > TCP_PROXY_WINDOW - f->tx_credit) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy: invalid WINDOW sid=%08x len=%zu credit=%u tx=%u", f->sid, len, credit, f->tx_credit);
        return -1;
    }
    f->tx_credit += credit;
    DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "proxy: WINDOW sid=%08x add=%u available=%u", f->sid, credit, f->tx_credit);
    return 0;
}

// Возвращает кредит через flush после передачи len байт локальному TCP (OS send/lwIP tcp_write).
// Владелец гарантирует, что байты ранее приняты и не учтены повторно; проверки здесь нет.
static inline void proxy_flow_consume(struct proxy_flow* f, uint32_t len) {
    f->consumed += len;
    proxy_flow_flush(f);
}
#endif
