/* Exit TCP-прокси: принимает CONNECT/DATA через UTUN-router и открывает обычный TCP к назначению.
 * Поток определяется группой, Ed25519 пира и stream_id; proxy_flow согласует окна и half-close.
 * init учитывает tcp_proxy_server_enabled в конфиге; destroy закрывает только TCP-потоки и свой binding.
 * Сначала init, затем приём через router callback, в конце destroy; повторный init живого сервера недопустим.
 * Состояние принадлежит UTUN_INSTANCE, операции и callbacks выполняются в его uasync-потоке.
 * CLOSE дренирует запись и прекращает чтение; ERROR немедленно уничтожает поток, FIN — half-close. */
#ifndef TCP_PROXY_SERVER_H
#define TCP_PROXY_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include "../lib/socket_compat.h"
#include "../lib/ll_queue.h"
#include "../lib/tcp_io.h"
#include "../routing_layer/etcp_router.h"

struct UTUN_INSTANCE;
struct UASYNC;
struct ll_entry;
struct ETCP_CONN;

#include "proxy_protocol.h"

struct tcp_proxy_server_conn {
    struct tcp_proxy_server_conn* next; // следующий поток exit
    struct tcp_proxy_server*      ctx;  // заимствованный владелец в inst
    struct tcp_conn*              tc;   // собственный OS TCP к назначению
    struct proxy_flow flow;             // окно, FIN и control retry
    uint32_t stream_id;                 // ID клиента; уникален в пределах группы и пира
    uint8_t  dest_ip[4];                // IPv4 назначения в сетевом порядке
    uint16_t dest_port;                 // порт назначения в сетевом порядке

    uint8_t  close_queued;     // close-маркер уже поставлен в write_queue TCP
    uint8_t  cli_closed;       // client sent CLOSE

    void*    diag_timer;       // 1-секундный таймер диагностики
    uint8_t  freed;            // логически удалён; сама структура освобождается через call_soon
    struct timeout_node* deferred_free; // Резерв на время закрытия, принадлежит опубликованному потоку.

    struct queue_waiter_handle pause_waiter; // ожидание свободного места в router send_q
    void*    retry_timer;      // fallback 500 мс: возобновить read_queue, DATA отправляется с force=0
    uint32_t bytes_sent;       // сумма входящих DATA, учитывается до проверки/записи в TCP
    uint32_t bytes_relayed;    // байт принято router для клиента
    uint32_t drain_count;      // число успешно отправленных DATA из read_queue
    uint32_t data_count;       // счётчик входящих DATA от клиента
    struct UASYNC* ua;         // заимствованный loop для таймеров и отложенного free
};

struct tcp_proxy_server {
    int enabled;                         // exit обслуживает CONNECT/UDP/ICMP REQUEST
    int conn_count;                      // число потоков в conns
    struct tcp_proxy_server_conn* conns; // собственные потоки всех пиров
    struct UTUN_INSTANCE* inst;          // заимствованный instance, содержащий сервер
};

// Обнулить состояние и учесть config->global.tcp_proxy_server_enabled; NULL inst → -1, иначе 0.
// Без конфига/при отключении не bind-ит сервис. При включении bind SERVER обязателен; отказ возвращает -1.
// Общими UDP/ICMP владеет instance; init/destroy TCP-сервера их не затрагивают.
int  tcp_proxy_server_init(struct UTUN_INSTANCE* inst);
// Удалить потоки, выключить сервер и снять свой binding; NULL inst и повторный destroy допустимы.
// Дать ua обработать отложенные освобождения до уничтожения instance.
void tcp_proxy_server_destroy(struct UTUN_INSTANCE* inst);

// Линейный поиск в ненулевом ctx по группе, пиру и sid; заимствованный указатель или NULL.
struct tcp_proxy_server_conn* tcp_proxy_server_find_conn(struct tcp_proxy_server* ctx, uint64_t group, const uint8_t peer[32], uint32_t stream_id);
// Немедленно удалить rc из списка, закрыть TCP и отменить таймеры/waiter; NULL допустим.
// Память rc освобождается отложенно в ua; сама функция не отправляет CLOSE/ERROR пиру.
void tcp_proxy_server_conn_free(struct tcp_proxy_server_conn* rc);

// Router callback: conn->instance — владелец, peer берётся из ROUTER_SVC_SRC_ED_OFF entry.
// Принимает владение entry/dgram при любом исходе. Пустой payload router удаляет потоки этого peer.
void tcp_proxy_server_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry);

// Зарегистрировать свободный сервис и CHAT-политику роутера. 0 — успех, -1 — ошибка с откатом регистрации.
int proxy_bind_service(struct UTUN_INSTANCE* inst, uint8_t service, etcp_recv_fn receive);

// Проверить общую квоту и квоту конечного peer. 1 разрешает создание, 0 означает отказ.
// Вызывать в uasync перед созданием новой записи, до её публикации не вызывать код пира.
int proxy_exit_can_open(const struct UTUN_INSTANCE* inst, const uint8_t peer[32]);

#ifdef __cplusplus
}
#endif
#endif
