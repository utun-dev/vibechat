/* stcp_server — TCP listener и входящий STCP-handshake для одного UTUN_INSTANCE.
 * create → connect_cb для готовых conn → destroy; сервер владеет listener и списком принятых соединений.
 * destroy закрывает также принятые conn и вызывает их close callbacks; все операции — uasync-поток.
 * set_reality включает TLS-камуфляж и relay неавторизованных клиентов; результат conn callback заимствован. */
#ifndef STCP_SERVER_H
#define STCP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "stcp.h"

struct UTUN_INSTANCE;

typedef void (*stcp_connect_cb)(struct stcp_conn *conn, void *arg);
typedef void (*stcp_close_cb)(struct stcp_conn *conn, int err, void *arg);

/* Создать listener на всех адресах family: AF_INET6 — IPv6-only, иначе IPv4; port=0 выбирает ОС.
 * ua/keys/connect_cb обязательны; X25519-ключи и соответствующий им обязательный my_ed25519_pubkey[32] копируются.
 * inst заимствован до destroy для padding/эпох ETCP; NULL допустим для самостоятельного STCP.
 * connect_cb(conn,arg) получает готовый заимствованный conn, close_cb(conn,err,close_arg) — его закрытие.
 * Результат — владеющий server handle или NULL при ошибке; принятые conn освобождает серверный STCP. */
struct stcp_server *stcp_server_create(struct UASYNC *ua, uint16_t port,
                                        struct SC_MYKEYS *keys,
                                        const uint8_t *my_ed25519_pubkey,
                                        struct UTUN_INSTANCE *inst,
                                        stcp_connect_cb connect_cb, void *arg,
                                        stcp_close_cb close_cb, void *close_arg,
                                        int family);
// Фактический TCP-порт слушателя (port=0 при create выбирает ОС); 0 при ошибке.
uint16_t stcp_server_get_port(const struct stcp_server *srv);
/* Закрыть listener и все принятые conn с их close callbacks; освободить srv. NULL допустим. */
void stcp_server_destroy(struct stcp_server *srv);

// Включить REALITY-камуфляж на этом TCP-сервере: перед STCP-хендшейком сервер
// ждёт ClientHello, проверяет авторизацию; неавторизованных проксирует на dest.
void stcp_server_set_reality(struct stcp_server *srv, const struct reality_config *rc);


#ifdef __cplusplus
}
#endif
#endif
