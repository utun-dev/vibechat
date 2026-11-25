/* stcp_client — асинхронные TCP connect и STCP-handshake, с опциональными SOCKS5 и REALITY.
 * connect возвращает владеющий handle или NULL; готовность DATA сообщает ready_cb, закрытие — close_cb.
 * get_conn заимствует conn из handle. destroy отменяет подключение/закрывает conn и делает handle недействительным.
 * Все API/callbacks — uasync-поток; TCP-ping использует отдельный короткий handshake и не создаёт ETCP-сессию. */
#ifndef STCP_CLIENT_H
#define STCP_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif


#include "stcp.h"
#include "socks_client.h"

typedef void (*stcp_ready_cb)(struct stcp_conn *conn, void *arg);
typedef void (*stcp_close_cb)(struct stcp_conn *conn, int err, void *arg);

/* Подключиться к addr:port (getaddrinfo для прямого TCP; SOCKS5 CONNECT при socks!=NULL).
 * ua/addr/keys/peer_pubkey[32] обязательны; нужен хотя бы ready_cb либо ping_cb.
 * X25519-ключи и соответствующий им обязательный my_ed25519_pubkey[32] копируются. etcp_conn заимствован:
 * handshake читает его текущие got_initial_pkt/reset_id; без него используется аргумент got_initial_pkt и эпоха 0.
 * device_type/keepalive_interval (мс)/hs_flags передаются в handshake; callbacks получают соответствующие arg.
 * local_addr — необязательный bind прямого сокета, reality/socks — необязательные внешние транспорты.
 * timeout_ms>0 задаёт таймаут подключения/handshake, иначе STCP_CONNECT_TIMEOUT (10 с).
 * inst нужен только при запуске для padding и времени REALITY, клиент не хранит ссылку на него.
 * Результат — владеющий handle либо NULL при отказе запуска; последующие ошибки приходят в close_cb. */
struct stcp_client *stcp_client_connect(struct UASYNC *ua, const char *addr, uint16_t port,
                                         struct SC_MYKEYS *keys, const uint8_t *peer_pubkey,
                                         const uint8_t *my_ed25519_pubkey,
                                         uint8_t got_initial_pkt,
                                         struct ETCP_CONN *etcp_conn,
                                         uint8_t device_type, uint16_t keepalive_interval,
                                         uint8_t hs_flags,
                                         stcp_ready_cb ready_cb, void *arg,
                                         stcp_ping_cb ping_cb, void *ping_arg,
                                         stcp_close_cb close_cb, void *close_arg,
                                         const struct sockaddr_storage *local_addr,
                                         int timeout_ms,
                                         const struct reality_client_config *reality,
                                         const struct socks_cfg *socks,
                                         struct UTUN_INSTANCE *inst);
/* TCP-ping: подключается и шлёт хендшейк с флагом STCP_HANDSHAKE_FLAG_PING.
 * Ответчик отвечает обычным хендшейк-ответом и мягко закрывает соединение.
 * По завершении хендшейка вызывается cb(success=1, rtt в 0.1 мс, arg); при ошибке/таймауте — cb(0, 0, arg).
 * NULL — отказ запуска без callback. Успешно созданная проба освобождается автоматически;
 * возвращённый handle служит для ранней отмены через destroy, после завершения пробы недействителен. */
struct stcp_client *stcp_ping_send(struct UASYNC *ua, const char *addr, uint16_t port,
                                    struct SC_MYKEYS *keys, const uint8_t *peer_pubkey,
                                    const uint8_t *my_ed25519_pubkey,
                                    uint8_t device_type, uint16_t keepalive_interval,
                                    int timeout_ms, stcp_ping_cb cb, void *arg,
                                    const struct socks_cfg *socks,
                                    struct UTUN_INSTANCE *inst, const struct reality_client_config *reality);
/* Отменить dial/закрыть conn и освободить handle; во время close освобождение откладывается. NULL допустим. */
void stcp_client_destroy(struct stcp_client *cli);
/* Заимствованный встроенный conn до destroy, доступен также до DATA и после закрытия I/O; NULL для cli==NULL. */
struct stcp_conn *stcp_client_get_conn(struct stcp_client *cli);


#ifdef __cplusplus
}
#endif
#endif
