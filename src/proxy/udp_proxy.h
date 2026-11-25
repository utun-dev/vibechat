/* UDP из TUN через UTUN-router: клиент отправляет REQUEST, exit открывает сокет для кортежа адресов
 * и возвращает REPLY. Потоки exit удаляются по таймауту неактивности.
 * init/destroy и callbacks — в uasync; inst владеет контекстом в inst->udp_proxy.
 * Приём REQUEST проверяет текущее server.enabled,
 * а REPLY — наличие tcp_proxy_client и совпадение peer с Ed25519 exit. Один inst может быть и клиентом, и exit.
 * API принимает IP/порты в network byte order, payload заимствован только на время вызова. */
#ifndef UDP_PROXY_H
#define UDP_PROXY_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include "../lib/socket_compat.h"
#include "../routing_layer/etcp_router.h"

struct UTUN_INSTANCE;
struct UASYNC;
struct ll_entry;
struct ETCP_CONN;

// Подкоманды
#define UDP_PROXY_SUBCMD_REQUEST 0x01
#define UDP_PROXY_SUBCMD_REPLY   0x02

// Сообщение для etcp_route_send (не полный wire packet; метаданные приёма добавляет router):
// svc_id(1) + subcmd(1) + src_ip(4) + src_port(2) + dst_ip(4) + dst_port(2) + payload
#define UDP_PROXY_HDR_SIZE       14
// Полный заголовок на приёме (после router_deliver):
// router header(26: svc/src/dst/flags/group) + subcmd(1) + src_ip(4) + src_port(2) + dst_ip(4) + dst_port(2)
#define UDP_PROXY_RECV_HDR_SIZE  (ROUTER_SVC_PAYLOAD_OFF + 13)   // 39

// Один UDP поток (сторона exit узла: сопоставляет client_ed + кортеж адресов с OS сокетом)
struct udp_flow {
    struct udp_flow* next;      // следующий flow exit
    struct udp_proxy_ctx* ctx;  // заимствованный владелец
    uint64_t group;             // Группа исходного запроса и обратной доставки.
    uint8_t client_ed[32];    // конечный ingress-узел
    uint32_t src_ip;            // исходный адрес приложения, network byte order
    uint16_t src_port;          // исходный порт приложения, network byte order
    uint32_t dst_ip;            // адрес назначения, network byte order
    uint16_t dst_port;          // порт назначения, network byte order
    socket_t sock;             // собственный неблокирующий UDP, bind ANY:0
    void*    read_id;           // регистрация сокета в ua
    struct UASYNC* ua;          // заимствованный event loop
    uint64_t created_tb;        // время создания (tb = 0.1 мс)
    uint64_t last_activity_tb;  // последний REQUEST или ответ от ожидаемого dst_ip:dst_port
};

struct udp_proxy_ctx {
    struct UTUN_INSTANCE* inst; // заимствованный instance-владелец
    struct UASYNC* ua;          // заимствованный event loop
    struct udp_flow* flows;     // собственные flows exit
    int          flow_count;    // число flows
    uint64_t     flow_timeout_tb; // допустимый простой потока, 300000 tb (30 с)
    void*        expire_timer;     // общий таймер ближайшего истечения, NULL при пустом списке
};


// Создать per-instance контекст и bind UDP service; повторный init существующего контекста даёт 0.
// inst/ua должны жить до destroy. 0 = контекст и binding готовы, -1 = ошибка без публикации нового контекста.
// TCP client/server этот контекст не создают/уничтожают: им владеет instance.
int udp_proxy_init(struct UTUN_INSTANCE* inst, struct UASYNC* ua);
// Снять binding, отменить таймер, закрыть flows и обнулить inst->udp_proxy; NULL/нет контекста допустимы.
void udp_proxy_destroy(struct UTUN_INSTANCE* inst);

// Exit узел: принимает REQUEST, создаёт сокет, шлёт данные
// Сторона клиента: принимает REPLY от настроенного exit, доставляет на TUN.
// conn->instance задаёт владельца; callback принимает владение entry/dgram при любом исходе.
void udp_proxy_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry);

// Скопировать payload (0..65507 байт) в REQUEST и отправить в UTUN, force=0/mode=0.
// Требует inst->udp_proxy; при payload_len>0 payload не NULL. 0 = принято router, <0 = ошибка.
int udp_proxy_send_to_exit(struct UTUN_INSTANCE* inst, const uint8_t exit_ed[32],
                           uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port,
                           const uint8_t* payload, size_t payload_len);

// Собрать IPv4/UDP src→dst и поставить в tun->input_queue (префикс версии 4); UDP checksum=0.
// Требует inst->tcp_proxy_client->tun; payload=0..65506. 0 = поставлено в очередь, -1 = ошибка.
int udp_proxy_deliver_reply(struct UTUN_INSTANCE* inst,
                            uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port,
                            const uint8_t* payload, size_t payload_len);


#ifdef __cplusplus
}
#endif
#endif // UDP_PROXY_H
