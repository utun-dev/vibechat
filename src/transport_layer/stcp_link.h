/* stcp_link — адаптер STCP-соединения к ETCP_LINK: handshake, очереди DATA, ready/close callbacks.
 * Серверный путь создаёт входящие линки, connect запускает исходящий для заданного ETCP_LINK.
 * send копирует data в tx_queue (0 / -1); ready означает готовность STCP, не READY topo-группы.
 * Адаптер владеет RX/TX очередями; stcp_conn их заимствует до закрытия.
 * close сразу запрещает send/RX/READY, гасит callback внезапного разрыва и откладывает teardown;
 * повторный close внутри активного callback допустим, после возврата в poll handle не использовать.
 * API и callbacks — uasync-поток. Указатели get_* заимствованы; адреса peer/local используют static-буферы. */
#ifndef STCP_LINK_H
#define STCP_LINK_H

#ifdef __cplusplus
extern "C" {
#endif


#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/socket_compat.h"
#include <stdint.h>

struct stcp_link;   // opaque
struct stcp_server; // opaque
struct ETCP_CONN;
struct ETCP_LINK;
struct ETCP_SOCKET;
struct UTUN_INSTANCE;

// ====== Link config ======

struct stcp_link_config {
    struct UTUN_INSTANCE *inst;       // rx-диспатч + instance_find_conn; ua/my_keys/ed25519 выводятся отсюда
    int listen_family;                // AF_INET или AF_INET6 для сервера (0 = v4)
    uint8_t reality_enabled;          // 1 = включить REALITY-камуфляж на TCP-сервере
};

// ====== TCP server ======

typedef void (*stcp_server_on_link_cb)(struct stcp_link *link, struct ETCP_SOCKET *tcp_sock);

struct stcp_server *stcp_server_listen(struct stcp_link_config *cfg, uint16_t port,
                                        stcp_server_on_link_cb on_link, struct ETCP_SOCKET *tcp_sock);
void stcp_link_server_destroy(struct stcp_server *srv);
// Фактический порт слушателя адаптера (включая listen с port=0); 0 при ошибке.
uint16_t stcp_link_server_get_port(const struct stcp_server *srv);

/* Server list management (multiple servers per instance) */
void  stcp_server_list_add(struct UTUN_INSTANCE *inst, struct stcp_server *srv);
void  stcp_server_list_remove(struct UTUN_INSTANCE *inst, struct stcp_server *srv);
void  stcp_server_list_destroy_all(struct UTUN_INSTANCE *inst);
int   stcp_server_list_count(struct UTUN_INSTANCE *inst);
struct stcp_server *stcp_server_list_find_by_sock(struct UTUN_INSTANCE *inst, const struct ETCP_SOCKET *sock);

// ====== TCP client link ======

/* NULL — запуск не состоялся, callback не вызывается. Принятый connect уведомляет также об ошибке до READY.
 * Все уведомления после возврата connect; владелец устанавливает callbacks до следующего poll. */
struct stcp_link *stcp_link_connect(struct ETCP_LINK *etcp_link,
                                    struct sockaddr_storage *remote_addr, uint16_t port);
/* Сразу отменяет работу и отвязывает владельца; освобождает handle через call_soon, без on_close. */
void stcp_link_close(struct stcp_link *link);
int  stcp_link_send(struct stcp_link *link, const uint8_t *data, size_t len);
/* Пакетный путь: готовность собственной TX-очереди адаптера (один ожидающий пакет).
 * send_entry: packet_link_result; только OK передаёт владение. Старый send копирует данные. */
struct ll_entry;
int stcp_link_can_send(struct stcp_link *link);
int stcp_link_send_entry(struct stcp_link *link, struct ll_entry *packet);
void stcp_link_set_rx_enabled(struct stcp_link *link, int enabled);
int  stcp_link_is_ready(struct stcp_link *link);
/* 1 — действующее соединение использует REALITY, 0 — обычный STCP либо NULL. Только uasync. */
int stcp_link_uses_reality(struct stcp_link* link);

struct ETCP_CONN *stcp_link_get_etcp_conn(struct stcp_link *link);
/* Заимствованный физический владелец; NULL после close. Проверять после callback до доступа к ETCP_LINK. */
struct ETCP_LINK *stcp_link_get_etcp_link(struct stcp_link *link);
void stcp_link_set_etcp_conn(struct stcp_link *link, struct ETCP_CONN *conn);
void stcp_link_set_etcp_link(struct stcp_link *link, struct ETCP_LINK *elink);
const struct sockaddr_storage *stcp_link_get_remote_addr(struct stcp_link *link);
const struct sockaddr_storage *stcp_link_get_peer_addr(struct stcp_link *link);  /* getpeername(conn->sock) */
const struct sockaddr_storage *stcp_link_get_local_addr(struct stcp_link *link);  /* getsockname(conn->sock) */

typedef void (*stcp_link_cb)(struct stcp_link *link, void *arg);
void stcp_link_set_on_ready(struct stcp_link *link, stcp_link_cb cb, void *arg);
void stcp_link_set_on_close(struct stcp_link *link, void (*cb)(struct stcp_link *link, int err, void *arg), void *arg);
const uint8_t *stcp_link_get_peer_pubkey(struct stcp_link *link);
const uint8_t *stcp_link_get_peer_ed25519_pubkey(struct stcp_link *link);
uint64_t stcp_link_get_peer_reset_id(struct stcp_link *link);
uint8_t  stcp_link_get_peer_device_type(struct stcp_link *link);
uint16_t stcp_link_get_peer_keepalive_interval(struct stcp_link *link);


#ifdef __cplusplus
}
#endif
#endif
