/* Автономный UDP-транспорт: TX/RX I/O и общие пулы криптографии без ETCP-объектов.
 * API/callback — основной uasync. Сокеты заимствованы: close снимает обслуживание и ждёт crypto,
 * после возврата владелец может закрыть fd. События содержат собственные значения, не ссылки на линк.
 * threads=0 сохраняет работу соответствующего направления в основном uasync. */
#ifndef UTUN_PACKET_TRANSPORT_H
#define UTUN_PACKET_TRANSPORT_H
#include "secure_channel.h"
#include "../../lib/socket_compat.h"
#include "transport_lane.h"
struct packet_transport;
struct packet_socket;
enum packet_event_type { PACKET_TRANSPORT_TX, PACKET_TRANSPORT_RX };
struct packet_transport_tag {
    uint64_t peer_id, epoch, attempt_id; // Непрозрачная корреляция с попыткой ETCP, 0 для control.
    uint8_t link_id;
};
struct packet_transport_event {
    enum packet_event_type type;
    uint64_t socket_generation, received_us;
    struct sockaddr_storage peer;
    struct packet_transport_tag tag;
    int error, init;           // error=0 при успехе; init=1 — проверен открытый хвост с публичным ключом.
    struct secure_channel crypto; // Снимок без pk; для INIT вернуть pk локального экземпляра перед использованием.
    const uint8_t *data;       // Расшифрованный пакет; заимствован только до возврата callback.
    size_t size, wire_size;
};
typedef void (*packet_transport_callback)(void *arg, const struct packet_transport_event *event);
/* Локальная идентичность копируется; callback/arg используются исключительно основным uasync. */
struct packet_transport *packet_transport_create(struct UASYNC *ua, unsigned tx, unsigned rx,
    const struct SC_MYKEYS *keys, packet_transport_callback callback, void *arg);
/* generation ненулевое и уникальное среди живых endpoint. fd уже nonblocking. NULL с логом при ошибке. */
struct packet_socket *packet_transport_socket(struct packet_transport *transport, socket_t fd, uint64_t generation, int socks);
/* Main: после смены адреса передать новое ненулевое поколение; уже принятые задания сохраняют прежнее. */
void packet_transport_set_generation(struct packet_socket *socket, uint64_t generation);
/* Только ядро: обновить RX-ключ по адресу. NULL удаляет ключ; принятые задания сохраняют свой снимок. */
int packet_transport_key(struct packet_socket *socket, const struct sockaddr_storage *peer, const struct secure_channel *key);
/* SOCKS: копия релея; NULL означает временную неготовность. Не меняет адрес назначения ETCP. */
void packet_transport_relay(struct packet_socket *socket, const struct sockaddr_storage *relay);
/* Копирует plaintext/tail/key/tag. 0 — принято, -2 — перегрузка/релей не готов, -1 — ошибка. */
int packet_transport_send(struct packet_socket *socket, const struct sockaddr_storage *peer, const struct secure_channel *key,
    const void *plain, size_t size, const void *tail, size_t tail_size, const struct packet_transport_tag *tag);
/* Текущий кредит отправки в этот endpoint, только основной uasync. До 3 незавершённых TX на (socket, peer);
 * кредит возвращается после обработки callback основным uasync. */
int packet_transport_ready(struct packet_socket *socket, const struct sockaddr_storage *peer);
/* main: разрешить/приостановить чтение UDP; принятые задания сохраняются, TX продолжает работать.
 * RX также автоматически ждёт общего кредита UDP/TCP перед recv; возврат кредита не отменяет паузу main. */
void packet_transport_receive(struct packet_socket *socket, int enabled);
/* Снять регистрации, завершить вычисления; не вызывает пользовательские callbacks и не закрывает fd. */
void packet_transport_close(struct packet_socket *socket);
/* Все сокеты должны быть закрыты. Удаляет ожидающие callbacks, завершает пулы и I/O. */
void packet_transport_destroy(struct packet_transport *transport);
/* Общие направления для TCP того же экземпляра; заимствованы до destroy. */
struct transport_lane *packet_transport_tx(struct packet_transport *transport);
struct transport_lane *packet_transport_rx(struct packet_transport *transport);
#endif
