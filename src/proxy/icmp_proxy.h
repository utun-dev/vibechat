/* IPv4 ICMP echo через UTUN-router: exit отправляет ping через raw socket, сопоставляет reply
 * по wire id/seq и IP цели, восстанавливая исходные id/seq клиента. Запросы ограничены таймаутом.
 * init/destroy и callbacks — в uasync; inst владеет контекстом в inst->icmp_proxy.
 * init регистрирует общий контекст; enable_exit отдельно открывает raw socket. recv_cb проверяет server.enabled для REQUEST
 * и Ed25519 exit клиентского proxy для REPLY. IP/id/seq в API — network byte order.
 * test_loopback явно переключает тестовый exit на виртуальные ответы, закрывая raw socket без pending-запросов. */
#ifndef ICMP_PROXY_H
#define ICMP_PROXY_H

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
#define ICMP_PROXY_SUBCMD_REQUEST 0x01  // client→exit: пингани dst_ip
#define ICMP_PROXY_SUBCMD_REPLY   0x02  // exit→client: echo reply

// Сообщение для etcp_route_send (не полный wire packet; метаданные приёма добавляет router):
// svc_id(1) + subcmd(1) + dst_ip(4) + orig_src_ip(4) + icmp_id(2) + icmp_seq(2) + payload
#define ICMP_PROXY_HDR_SIZE       14
// Полный заголовок на приёме (после router_deliver):
// router header(26: svc/src/dst/flags/group) + subcmd(1) + dst_ip(4) + orig_src_ip(4) + icmp_id(2) + icmp_seq(2)
#define ICMP_PROXY_RECV_HDR_SIZE  (ROUTER_SVC_PAYLOAD_OFF + 13)   // 39

// Активный ICMP echo запрос (сторона exit узла)
struct icmp_request {
    struct icmp_request* next; // следующий ожидающий запрос exit
    uint64_t group;             // Группа исходного запроса и обратной доставки.
    uint8_t client_ed[32];   // ingress-узел для обратной доставки
    uint32_t dst_ip;           // IP цели ping, network byte order
    uint32_t orig_src_ip;    // IP исходного отправителя (чтобы вернуть reply правильному адресату)
    uint16_t echo_id;          // исходный id приложения, network byte order
    uint16_t echo_seq;         // исходный seq приложения, network byte order
    uint16_t wire_id;          // заменённый id в raw ICMP, network byte order
    uint16_t wire_seq;         // заменённый seq, уникален вместе с wire_id и dst_ip среди pending
    uint8_t  payload[1500];    // сохранённый префикс запроса, не используется для сборки ответа
    size_t   payload_len;      // длина сохранённого префикса, не больше sizeof(payload)
    uint64_t sent_tb;          // время отправки, tb = 0.1 мс
    socket_t sock;             // не используется; запросы идут через общий ctx->raw_sock
};

struct icmp_proxy_ctx {
    struct UTUN_INSTANCE* inst; // заимствованный instance-владелец
    struct UASYNC* ua;         // заимствованный event loop
    socket_t     raw_sock;        // один SOCK_RAW для всего ICMP на exit
    void*        raw_read_id;   // регистрация raw_sock в ua
    struct icmp_request* pending; // собственные запросы, ожидающие ответа
    int pending_count;            // Число записей pending для общей квоты exit.
    uint32_t next_token;        // генератор wire id/seq со случайным начальным значением
    uint64_t     request_timeout_tb; // срок ожидания отдельного ответа, 50000 tb (5 с)
    int          test_loopback; // явный виртуальный exit, только для тестов
    void*        expire_timer;     // общий таймер ближайшего истечения, NULL при пустом списке
};


// Создать контекст и bind ICMP service; повторный init не меняет существующий контекст и даёт 0.
// inst/ua живут до destroy. 0 = контекст и binding готовы, <0 = ошибка без публикации нового контекста.
// Контекстом владеет instance; TCP client/server его не уничтожают. Raw socket здесь не создаётся.
int icmp_proxy_init(struct UTUN_INSTANCE* inst, struct UASYNC* ua);
// Запустить raw exit существующего контекста. 0 = готов (включая явный test_loopback), <0 = -errno.
// Отказ сохраняет клиентскую сторону и прежнее состояние; политику отсутствия прав выбирает instance.
int icmp_proxy_enable_exit(struct UTUN_INSTANCE* inst);
// Снять binding, отменить таймер, закрыть raw socket, удалить pending и обнулить inst->icmp_proxy; NULL допустим.
void icmp_proxy_destroy(struct UTUN_INSTANCE* inst);

// Router callback: conn->instance задаёт владельца; REQUEST требует server.enabled, REPLY — настроенного exit.
// Принимает владение entry/dgram при любом исходе, в том числе при отклонении пакета.
void icmp_proxy_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry);

// Скопировать payload (0..65507) в REQUEST к exit в UTUN, force=0/mode=0; не требует локального контекста.
// dst_ip — цель ping, orig_src_ip — адрес приложения; payload заимствован, при длине>0 не NULL.
// 0 = принято router, <0 = ошибка; это не подтверждение raw send или получения echo reply.
int icmp_proxy_send_to_exit(struct UTUN_INSTANCE* inst, const uint8_t exit_ed[32],
                            uint32_t dst_ip, uint32_t orig_src_ip, uint16_t echo_id, uint16_t echo_seq,
                            const uint8_t* payload, size_t payload_len);

// Собрать Echo Reply с IPv4 src_ip→dst_ip и исходными id/seq; пересчитать обе checksum.
// Требует inst->tcp_proxy_client->tun, payload заимствован (0..65506); 0 = в tun input_queue, -1 = ошибка.
int icmp_proxy_deliver_reply(struct UTUN_INSTANCE* inst,
                             uint32_t dst_ip, uint32_t src_ip, uint16_t echo_id, uint16_t echo_seq,
                             const uint8_t* payload, size_t payload_len);

// Явно включить тестовый loopback, закрыв raw socket; при pending-запросах изменение отклоняется с WARN.
// Без контекста ничего не делает; выключение loopback само по себе raw socket не открывает.
void icmp_proxy_set_test_loopback(struct UTUN_INSTANCE* inst, int enabled);


#ifdef __cplusplus
}
#endif
#endif // ICMP_PROXY_H
