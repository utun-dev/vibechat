/* Локальные SOCKS5 CONNECT и HTTP proxy/CONNECT: DNS, handshake и передача TCP через удалённый exit.
 * tcp_proxy_client владеет listener и списками соединений; их callbacks выполняются в uasync.
 * init_listen заимствует inst/ua/списки/счётчики; они живут до закрытия listener И освобождения клиентов.
 * Сначала закрыть listener, затем conn_free_all; закрытие listener само по себе не освобождает клиентов.
 * DNS выполняется локально (первая A-запись); SOCKS поддерживает только CONNECT без аутентификации.
 * HTTP keep-alive использует новый stream для каждой транзакции; CONNECT/101 переходят в сырой туннель. */
#ifndef SOCKS_PROXY_H
#define SOCKS_PROXY_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include "proxy_protocol.h"
#include "../lib/socket_compat.h"
#include "../lib/ll_queue.h"

struct UASYNC;
struct UTUN_INSTANCE;
struct ll_entry;
struct ll_queue;
struct tcp_conn;
struct adns_query;
struct socks_http_state;

enum {
    SOCKS_STATE_GREETING = 0,  // ждём SOCKS5 greeting (ver + methods)
    SOCKS_STATE_REQUEST,       // ждём SOCKS5 CONNECT request (== HTTP_STATE_REQUEST)
    SOCKS_STATE_CONNECTING,    // ждём DNS или CONNECTED от exit (== HTTP_STATE_CONNECTING)
    SOCKS_STATE_RELAY          // релей данных (== HTTP_STATE_RELAY)
};

// DNS/connect и сырой туннель используют общие состояния; HTTP framing хранится отдельно в http.
enum {
    HTTP_STATE_REQUEST    = SOCKS_STATE_REQUEST,     // ждём заголовки очередного HTTP-запроса
    HTTP_STATE_CONNECTING = SOCKS_STATE_CONNECTING,  // DNS или реальный connect на exit
    HTTP_STATE_RELAY      = SOCKS_STATE_RELAY        // релей данных
};

struct socks_proxy_conn {
    struct socks_proxy_conn* next;     // следующий клиент в списке владельца
    struct socks_proxy_conn** head;    // указатель на голову списка (для удаления)
    int*                count;         // указатель на счётчик списка
    struct tcp_conn*    tc;            // собственный OS TCP к локальному клиенту
    struct UASYNC*      ua;            // заимствованный event loop
    struct UTUN_INSTANCE* inst;        // заимствованный instance
    struct tcp_proxy_client* client;    // владелец общего лимита подключений и отправки
    struct proxy_flow flow;            // окно и control retry текущего upstream
    uint32_t            stream_id;     // текущий upstream; HTTP между транзакциями может быть 0
    uint32_t*           next_stream_id;  // HTTP: новый поток для следующей транзакции
    struct socks_http_state* http;     // собственное HTTP-состояние; NULL для SOCKS
    uint8_t             dest_ip[4];    // IPv4 назначения, network byte order
    uint16_t            dest_port;     // порт назначения, network byte order
    uint8_t             fin_remote;    // FIN от exit; FIN локального клиента хранится в tc->fin_remote
    uint8_t             rem_closed;    // прекращена обработка потока/клиента
    uint8_t             close_sent;    // CLOSE принят router
    uint8_t             close_pending; // неудавшийся CLOSE; последняя попытка при free
    uint8_t             state;         // SOCKS_STATE_* / HTTP_STATE_*
    uint8_t             freed;         // освобождение уже началось
    uint8_t             close_queued;  // close-маркер уже поставлен в write_queue
    struct timeout_node* cleanup;      // Резерв до постановки в очередь, затем handle для отмены
    uint8_t             free_pending;  // Освобождение уже поставлено в очередь
    uint8_t             buf[16384];    // handshake или неразобранный хвост read block
    uint16_t            buf_len;       // число сохранённых байтов в buf
    uint8_t*            tx_buf;    // буфер при backpressure (retry в tx_waiter_cb)
    uint16_t            tx_len;        // остаток tx_buf; отправляется блоками до 4096 байт
    uint8_t             sending;       // отправка фрагмента tx_buf активна, вложенный WINDOW только возвращает кредит
    uint32_t            bytes_to_client;  // байт записано клиенту (curl) через write_queue
    uint32_t            bytes_from_exit;  // байт получено от exit node через ETCP DATA
    void*               greeting_timer;  // срок SOCKS greeting/request/DNS
    struct timeout_node* drain_timer; // резерв для дописывания при терминальном закрытии, один срок на поток
    uint8_t             fin_deferred;    // зарезервировано, текущая реализация не использует
    struct adns_query*  dns_q;           // активный DNS-запрос (NULL когда нет)
    uint8_t             dns_kind;        // DNSK_SOCKS / DNSK_HTTP_CONNECT / DNSK_HTTP_PROXY
};

struct listen_ctx;

// Создать IPv4 listener: addr_str = "IPv4:PORT", 0 разрешает выбор порта ОС.
// conns/count/next_stream_id и inst/ua заимствуются; is_http=0 — SOCKS5, иначе HTTP.
// Общий генератор sid нужен для всех списков одного tcp_proxy_client. Возврат: ctx или NULL при ошибке.
// Не регистрирует router callback: входящие ответы должен передавать tcp_proxy_client.
struct listen_ctx* socks_proxy_init_listen(struct UASYNC* ua, const char* addr_str,
    struct socks_proxy_conn** conns, int* count, uint32_t* next_stream_id,
    struct tcp_proxy_client* client, int is_http);

// Закрыть listener и освободить ctx в том же ua. NULL ctx допустим; активные клиенты сохраняются.
// Если sock_out задан, в него записывается SOCKET_INVALID: сокет уже закрыт.
void socks_proxy_close_listen(struct UASYNC* ua, struct listen_ctx* ctx, socket_t* sock_out);
// Фактический порт активного listener (IP:0 выбирает ОС); 0 при ошибке.
uint16_t socks_proxy_get_port(const struct listen_ctx* ctx);

// Найти текущий stream_id в head; заимствованный результат или NULL, список не меняется.
struct socks_proxy_conn* socks_proxy_find_conn(struct socks_proxy_conn* head, uint32_t stream_id);

// Передать ответ exit: CONNECTED/DATA/WINDOW/CLOSE/ERROR/FIN; data заимствован на время вызова.
// Проверку peer/group выполняет внешний dispatcher. Может закрыть/удалить соединение.
// Возвращает 1 при найденном sid (в том числе неизвестная команда), 0 если sid не найден.
int socks_proxy_handle_etcp(struct socks_proxy_conn** head, int* count,
                            uint32_t stream_id, uint8_t subcmd,
                            const uint8_t* data, size_t data_len);

// Освободить c и его TCP/DNS/HTTP/таймеры/waiter, удалить из списка; NULL допустим.
// Активный HTTP upstream отменяется; в остальных случаях free не гарантирует уведомление exit.
void socks_proxy_conn_free(struct socks_proxy_conn* c);

// Освободить все соединения; head/count — те же живые адреса, что переданы init_listen.
// Освободить список; notify_exit=0 при сбросе исходящего канала, очередь которого уже закрывается.
void socks_proxy_conn_free_all(struct socks_proxy_conn** head, int notify_exit);


#ifdef __cplusplus
}
#endif
#endif // SOCKS_PROXY_H
