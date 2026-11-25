/* Клиент TCP-прокси: принимает IPv4 с отдельного TUN через lwIP либо TCP от SOCKS/HTTP listener.
 * Передаёт потоки через UTUN-router на Ed25519 via_ed; proxy_flow обеспечивает окно и backpressure.
 * create/destroy и callbacks — в uasync. Владеет созданными TUN/lwIP/listener/потоками,
 * заимствует inst/ua/mappings до destroy. Без пары непустых tun_name/tun_ip работает только SOCKS/HTTP.
 * Владелец сам записывает результат create в inst->tcp_proxy_client и обнуляет поле после destroy.
 * Клиент принимает TCP-ответы только от via_ed в TOPO_GROUP_UTUN; UDP/ICMP требуют TUN. */
#ifndef TCP_PROXY_CLIENT_H
#define TCP_PROXY_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include "proxy_protocol.h"
#include <stddef.h>
#include "../lib/socket_compat.h"
#include "../lib/ll_queue.h"

struct UASYNC;
struct UTUN_INSTANCE;
struct ETCP_CONN;
struct tun_if;
struct ll_queue;
struct ll_entry;
struct memory_pool;
struct tcp_proxy_client_mapping_config;
struct tcp_proxy_server;
struct lwip_tcp_ctx;
struct socks_proxy_conn;
struct listen_ctx;

struct tcp_pcb;

#define TCP_PROXY_CLIENT_OPEN_LIMIT 64      // ожидание места для CONNECT и ответа CONNECTED
#define TCP_PROXY_CLIENT_STREAM_LIMIT 256  // локальные сокеты, TUN-потоки и завершения, ожидающие отправки
#define TCP_PROXY_CLIENT_CONTROL_RESERVE 8 // места в router send_q сверх обычного порога

struct proxy_terminal {
    uint64_t deadline_tb;
    uint32_t sid;
    uint8_t cmd;
};

struct tcp_proxy_client_conn {
    struct tcp_proxy_client_conn* next; // следующий lwIP-поток клиента
    struct tcp_proxy_client*  proxy;    // заимствованный владелец
    struct tcp_pcb*           pcb;      // принадлежит lwIP; NULL после detach/err callback

    struct proxy_flow flow;            // окно и retry CONNECTED/WINDOW/FIN
    uint32_t stream_id;                // общий генератор с SOCKS/HTTP-потоками

    struct ll_queue* to_lwip;  // DATA от exit → lwIP (управление потоком)

    uint8_t  fin_local;        // локальная сторона отправила FIN (lwIP)
    uint8_t  fin_remote;       // exit отправил FIN (назначение закрылось)
    uint8_t  fin_deferred;     // FIN от exit пришёл, но отложен до дренажа to_lwip
    uint8_t  rem_closed;       // exit отправил CLOSE
    uint8_t  error;            // ошибка, немедленная очистка
    uint8_t  close_sent;       // отправили CLOSE/ERROR в exit
    uint8_t  close_pending;    // CLOSE/ERROR не принят router, повтор в poll/free пока pc жив
    uint64_t fin_wait_deadline_tb; // Дедлайн простоя FIN_WAIT_2. При приёме или передаче DATA начинается новый период.

    struct ll_queue* tx_queue;        // буфер lwIP→ETCP при backpressure (аналог server read_queue)

    uint32_t bytes_to_exit;    // байт принято router для exit (из lwIP/TUN)
    uint32_t bytes_from_exit;  // байт получено от exit (в lwIP/TUN)
    uint32_t bp_count;         // счётчик backpressure (send fail)

    uint8_t  dest_ip[4];       // IPv4 назначения в сетевом порядке
    uint16_t dest_port;        // порт назначения в сетевом порядке
};

struct tcp_proxy_client {
    struct UTUN_INSTANCE* inst; // заимствован; NULL допустим для автономного TUN smoke test
    uint8_t router_bound;      // собственный CLIENT binding установлен; rollback не снимает чужую регистрацию
    struct UASYNC*    ua;       // заимствованный event loop
    struct tun_if*    tun;      // собственный TUN или NULL для SOCKS/HTTP без TUN
    struct tcp_proxy_client_conn* conns; // список собственных lwIP-потоков
    int               conn_count;       // число lwIP-потоков, без SOCKS/HTTP
    struct memory_pool* entry_pool;     // собственный пул ll_entry для очередей lwIP-потоков
    uint32_t          next_stream_id;   // общий счётчик TUN/SOCKS/HTTP, init=1, перед выдачей инкрементируется
    uint8_t           via_ed[32];   // Полный адрес exit, определяет получателя и допустимый источник.
    struct lwip_tcp_ctx* lwip;          // собственный TCP-стек или NULL без TUN
    struct ll_queue* tx_tasks[PROXY_TX_TASKS]; // встроенные задачи по классам отправки
    struct queue_waiter_handle tx_waiter; // одно ожидание router send_q для всех потоков exit
    struct uasync_work tx_kick;                    // отложенный запуск диспетчера
    struct timeout_node* tx_retry;                   // общий повтор при отсутствии waiter или прогресса
    int tx_running;                  // диспетчер сейчас выполняет задачу
    int next_open;                   // чередование CONNECT и DATA
    unsigned opening;                // занятые места ожидания CONNECTED
    unsigned terminal_head, terminal_count;
    struct proxy_terminal terminals[TCP_PROXY_CLIENT_STREAM_LIMIT]; // завершения уже освобождённых потоков

    struct tcp_proxy_client_mapping_config* mappings;  // указатель на конфиг
    int               mapping_count;   // число записей mappings, порты конфига в host byte order

    // SOCKS proxy
    int               socks_enabled;    // SOCKS5 listener обязателен при создании
    struct socks_proxy_conn* socks_conns; // собственные SOCKS5-клиенты
    int               socks_conn_count;  // число SOCKS5-клиентов
    struct listen_ctx* socks_listen;     // собственный listener или NULL при отключении/ошибке

    // HTTP forward proxy и CONNECT
    int               http_proxy_enabled; // HTTP listener обязателен при создании
    struct socks_proxy_conn* http_conns;   // собственные HTTP-клиенты
    int               http_conn_count;    // число HTTP-клиентов
    struct listen_ctx* http_listen;       // собственный listener или NULL при отключении/ошибке
};

// Есть место для нового локального потока, с учётом TUN в SYN_RCVD и ещё не отправленных завершений.
int tcp_proxy_client_can_accept(const struct tcp_proxy_client* p);
// Начать CONNECT; 0 — принято, отрицательный errno — немедленный отказ. failed получает errno позже.
int tcp_proxy_client_open(struct proxy_flow* flow, const uint8_t ip[4], uint16_t port, void (*failed)(void*, int));
// Доставить CLOSE/ERROR для освобождаемого потока либо сохранить его в конечном буфере завершений.
void tcp_proxy_client_retire(struct tcp_proxy_client* p, uint32_t sid, uint8_t cmd);

// Создать клиент; ua обязателен. Непустые tun_name И tun_ip включают TUN/lwIP (mtu<=0 → 1500).
// test_mode передаётся tun_init_nat: ненулевой использует тестовый TUN; mappings заимствуются до destroy.
// SOCKS/HTTP создаются при enabled и непустом IPv4:port; via_ed — полный Ed25519 общего exit.
// Результат: собственный p или NULL при критической ошибке/отсутствии режима.
// Все выбранные входы и CLIENT binding обязательны; отказ полностью освобождает новые ресурсы.
// Общие UDP/ICMP создаёт и уничтожает instance отдельно; create их не затрагивает.
struct tcp_proxy_client* tcp_proxy_client_create(struct UTUN_INSTANCE* inst, struct UASYNC* ua,
    const char* tun_name, const char* tun_ip, int mtu, int test_mode,
    struct tcp_proxy_client_mapping_config* mappings, int mapping_count,
    const uint8_t via_ed[32],
    int socks_enabled, const char* socks_addr,
    int http_proxy_enabled, const char* http_proxy_addr);
// Остановить listeners, освободить потоки/TUN/lwIP и снять router binding; NULL допустим.
// Не затрагивает общие UDP/ICMP и не обнуляет inst->tcp_proxy_client.
void tcp_proxy_client_destroy(struct tcp_proxy_client* p);
// Router callback: conn->instance задаёт владельца, entry содержит метаданные router и передаёт владение.
// Освобождает entry/dgram при любом исходе; CLOSE_ALL = пустой payload router, а не TCP-подкоманда.
void tcp_proxy_client_router_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry);


#ifdef __cplusplus
}
#endif
#endif // TCP_PROXY_CLIENT_H
